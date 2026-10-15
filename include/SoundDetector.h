#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Pure detection logic, independent of the Windows audio APIs, so it can be unit
// tested with synthetic meter readings (see tests/test_detection.cpp).
//
// Times are milliseconds on a monotonic clock; peaks are linear 0.0 - 1.0 as
// reported by IAudioMeterInformation.

using TimeMs = int64_t;

// Case-insensitive match supporting '*' and '?'. Several patterns may be
// separated by ';' (e.g. "Wow.exe;WowClassic.exe").
bool WildcardMatch(const std::wstring& patterns, const std::wstring& text);

// Splits a session's meter readings into discrete sounds for the event log.
class SoundSegmenter {
public:
    enum class Change { None, Started, Ended };

    Change Update(TimeMs now, float peak, float threshold, int releaseMs);
    void Reset();

    bool Active() const { return m_active; }
    TimeMs StartTime() const { return m_start; }
    TimeMs LastLoudTime() const { return m_lastLoud; }
    float MaxPeak() const { return m_maxPeak; }
    TimeMs DurationMs() const { return m_lastLoud - m_start; }

    // Loudest reading per 100 ms bucket (first 60 s), as whole percentages.
    std::wstring EnvelopeText() const;

private:
    bool m_active = false;
    TimeMs m_start = 0;
    TimeMs m_lastLoud = 0;
    float m_maxPeak = 0.0f;
    std::vector<float> m_envelope;
};

struct AlertParams {
    float threshold = 0.2f;        // Peak level that counts as loud
    int quietBeforeMs = 1500;      // Required time below the threshold before a loud run
    int minLoudMs = 0;             // Run must stay loud this long before firing
    int cooldownMs = 3000;         // No further alert for this long after firing
    int suppressAfterInputMs = 0;  // Ignore runs this soon after user input (0 = off)
};

enum class AlertDecision {
    None,             // Nothing decided on this sample
    Fired,
    BlockedNotQuiet,  // Run started too soon after the previous loud moment
    BlockedCooldown,
    BlockedInput,     // User input happened just before (e.g. the cast sound)
    BlockedTooShort,  // Run ended before minLoudMs
};
const wchar_t* AlertDecisionName(AlertDecision decision);

// Decides when a session's audio should raise an alert. Every loud run (peak at
// or above the threshold, allowing brief dips) gets exactly one decision.
class AlertEvaluator {
public:
    static constexpr TimeMs kGapToleranceMs = 80;  // Dips shorter than this don't end a run

    struct Result {
        AlertDecision decision = AlertDecision::None;
        bool runStarted = false;
        TimeMs quietForMs = -1;   // Silence before this run; -1 = never loud before
        TimeMs loudForMs = 0;     // How long the run had been loud at decision time
        TimeMs cooldownLeftMs = 0;
        TimeMs inputAgeMs = -1;   // Time since the relevant user input; -1 = none seen
    };

    // lastInputMs: time of the most recent relevant user input, or -1 if none.
    Result Update(TimeMs now, float peak, TimeMs lastInputMs, const AlertParams& params);
    void Reset() { *this = AlertEvaluator(); }

    std::wstring StateText(TimeMs now, TimeMs lastInputMs, const AlertParams& params) const;
    bool IsLoud() const { return m_loud; }

private:
    bool m_loud = false;
    bool m_decided = false;
    TimeMs m_lastAbove = -1;
    TimeMs m_aboveSince = 0;
    TimeMs m_quietFor = -1;
    TimeMs m_cooldownUntil = -1;
};
