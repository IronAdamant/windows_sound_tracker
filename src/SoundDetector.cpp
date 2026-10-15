#include "SoundDetector.h"

#include <algorithm>
#include <cwctype>

namespace {

bool MatchOne(const wchar_t* p, const wchar_t* pEnd, const wchar_t* t, const wchar_t* tEnd) {
    const wchar_t* starP = nullptr;
    const wchar_t* starT = nullptr;
    while (t < tEnd) {
        if (p < pEnd && (*p == L'?' || std::towlower(*p) == std::towlower(*t))) {
            ++p;
            ++t;
        } else if (p < pEnd && *p == L'*') {
            starP = p++;
            starT = t;
        } else if (starP) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pEnd && *p == L'*') ++p;
    return p == pEnd;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

}  // namespace

bool WildcardMatch(const std::wstring& patterns, const std::wstring& text) {
    size_t start = 0;
    while (start <= patterns.size()) {
        size_t end = patterns.find(L';', start);
        if (end == std::wstring::npos) end = patterns.size();
        std::wstring pattern = Trim(patterns.substr(start, end - start));
        if (!pattern.empty() &&
            MatchOne(pattern.data(), pattern.data() + pattern.size(), text.data(), text.data() + text.size())) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

SoundSegmenter::Change SoundSegmenter::Update(TimeMs now, float peak, float threshold, int releaseMs) {
    Change change = Change::None;
    if (peak >= threshold) {
        if (!m_active) {
            m_active = true;
            m_start = now;
            m_maxPeak = 0.0f;
            m_envelope.clear();
            change = Change::Started;
        }
        m_lastLoud = now;
        m_maxPeak = (std::max)(m_maxPeak, peak);
    } else if (m_active && now - m_lastLoud >= releaseMs) {
        m_active = false;
        return Change::Ended;
    }
    if (m_active) {
        size_t bucket = static_cast<size_t>((now - m_start) / 100);
        if (bucket < 600) {
            if (m_envelope.size() <= bucket) m_envelope.resize(bucket + 1, 0.0f);
            m_envelope[bucket] = (std::max)(m_envelope[bucket], peak);
        }
    }
    return change;
}

void SoundSegmenter::Reset() {
    m_active = false;
    m_envelope.clear();
}

std::wstring SoundSegmenter::EnvelopeText() const {
    std::wstring out;
    for (size_t i = 0; i < m_envelope.size(); ++i) {
        if (i) out += L' ';
        out += std::to_wstring(static_cast<int>(m_envelope[i] * 100.0f + 0.5f));
    }
    return out;
}

const wchar_t* AlertDecisionName(AlertDecision decision) {
    switch (decision) {
        case AlertDecision::None: return L"none";
        case AlertDecision::Fired: return L"FIRED";
        case AlertDecision::BlockedNotQuiet: return L"blocked: not quiet long enough before";
        case AlertDecision::BlockedCooldown: return L"blocked: cooldown";
        case AlertDecision::BlockedInput: return L"blocked: user input just before";
        case AlertDecision::BlockedTooShort: return L"blocked: too short";
    }
    return L"?";
}

AlertEvaluator::Result AlertEvaluator::Update(TimeMs now, float peak, TimeMs lastInputMs, const AlertParams& p) {
    Result r;
    if (peak >= p.threshold) {
        if (!m_loud) {
            m_loud = true;
            m_decided = false;
            m_aboveSince = now;
            m_quietFor = m_lastAbove < 0 ? -1 : now - m_lastAbove;
            r.runStarted = true;
            r.quietForMs = m_quietFor;
            if (m_quietFor >= 0 && m_quietFor < p.quietBeforeMs) {
                m_decided = true;
                r.decision = AlertDecision::BlockedNotQuiet;
            }
        }
        m_lastAbove = now;
        if (!m_decided && now - m_aboveSince >= p.minLoudMs) {
            m_decided = true;
            r.quietForMs = m_quietFor;
            r.loudForMs = now - m_aboveSince;
            r.inputAgeMs = lastInputMs >= 0 ? now - lastInputMs : -1;
            if (m_cooldownUntil >= 0 && now < m_cooldownUntil) {
                r.decision = AlertDecision::BlockedCooldown;
                r.cooldownLeftMs = m_cooldownUntil - now;
            } else if (p.suppressAfterInputMs > 0 && lastInputMs >= 0 && now - lastInputMs < p.suppressAfterInputMs) {
                r.decision = AlertDecision::BlockedInput;
            } else {
                r.decision = AlertDecision::Fired;
                m_cooldownUntil = now + p.cooldownMs;
            }
        }
    } else if (m_loud && now - m_lastAbove > kGapToleranceMs) {
        m_loud = false;
        if (!m_decided) {
            m_decided = true;
            r.decision = AlertDecision::BlockedTooShort;
            r.quietForMs = m_quietFor;
            r.loudForMs = m_lastAbove - m_aboveSince;
        }
    }
    return r;
}

std::wstring AlertEvaluator::StateText(TimeMs now, TimeMs lastInputMs, const AlertParams& p) const {
    if (m_loud) return L"Loud";
    if (m_cooldownUntil >= 0 && now < m_cooldownUntil) {
        return L"Cooldown " + std::to_wstring((m_cooldownUntil - now + 999) / 1000) + L"s";
    }
    if (p.suppressAfterInputMs > 0 && lastInputMs >= 0 && now - lastInputMs < p.suppressAfterInputMs) {
        return L"Ignoring (input)";
    }
    if (m_lastAbove >= 0 && now - m_lastAbove < p.quietBeforeMs) return L"Waiting for quiet";
    return L"Armed";
}
