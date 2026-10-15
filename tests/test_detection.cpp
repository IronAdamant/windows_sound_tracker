// Unit tests for the pure detection logic (no audio devices needed).
// Run: ctest --test-dir <build> --output-on-failure

#include <cstdio>
#include <random>
#include <vector>

#include "SoundDetector.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

// A sound in a synthetic timeline: [start, start+duration) at a constant level.
struct Tone {
    TimeMs start;
    TimeMs duration;
    float level;
};

struct Run {
    std::vector<TimeMs> fired;
    std::vector<AlertDecision> decisions;
};

// Feeds samples every 'step' ms (optionally jittered) through an evaluator.
Run Simulate(const std::vector<Tone>& tones, TimeMs endMs, const AlertParams& params, TimeMs step = 20,
             const std::vector<TimeMs>& inputs = {}, unsigned jitterSeed = 0) {
    AlertEvaluator evaluator;
    Run run;
    std::mt19937 rng(jitterSeed);
    std::uniform_int_distribution<int> jitter(-8, 15);
    for (TimeMs t = 0; t <= endMs;) {
        float peak = 0.0f;
        for (const auto& tone : tones) {
            if (t >= tone.start && t < tone.start + tone.duration) peak = tone.level > peak ? tone.level : peak;
        }
        TimeMs lastInput = -1;
        for (TimeMs in : inputs) {
            if (in <= t) lastInput = in;
        }
        auto r = evaluator.Update(t, peak, lastInput, params);
        if (r.decision != AlertDecision::None) run.decisions.push_back(r.decision);
        if (r.decision == AlertDecision::Fired) run.fired.push_back(t);
        t += step + (jitterSeed ? jitter(rng) : 0);
    }
    return run;
}

AlertParams Fishing() {
    AlertParams p;
    p.threshold = 0.20f;
    p.quietBeforeMs = 1500;
    p.minLoudMs = 0;
    p.cooldownMs = 4000;
    p.suppressAfterInputMs = 0;
    return p;
}

void TestWildcard() {
    std::printf("wildcard matching\n");
    CHECK(WildcardMatch(L"Wow.exe", L"Wow.exe"));
    CHECK(WildcardMatch(L"wow.exe", L"WOW.EXE"));
    CHECK(WildcardMatch(L"Wow*.exe", L"WowClassic.exe"));
    CHECK(!WildcardMatch(L"Wow.exe", L"WowVoiceProxy.exe"));
    CHECK(WildcardMatch(L"Wow.exe;WowClassic.exe", L"WowClassic.exe"));
    CHECK(WildcardMatch(L" Wow.exe ; chrome.exe ", L"chrome.exe"));
    CHECK(WildcardMatch(L"Wow?.exe", L"WowT.exe"));
    CHECK(!WildcardMatch(L"Wow?.exe", L"Wow.exe"));
    CHECK(WildcardMatch(L"*", L"anything.exe"));
    CHECK(!WildcardMatch(L"", L"Wow.exe"));
    CHECK(!WildcardMatch(L";;", L"Wow.exe"));
}

void TestSplashFiresOnce() {
    std::printf("splash after silence fires exactly once\n");
    auto run = Simulate({{5000, 800, 0.5f}}, 10000, Fishing());
    CHECK(run.fired.size() == 1);
    CHECK(!run.fired.empty() && run.fired[0] >= 5000 && run.fired[0] <= 5020);
}

void TestQuietCastIgnored() {
    std::printf("quiet cast below threshold never fires, splash does\n");
    auto run = Simulate({{1000, 450, 0.08f}, {7000, 800, 0.5f}}, 10000, Fishing());
    CHECK(run.fired.size() == 1);
    CHECK(!run.fired.empty() && run.fired[0] >= 7000 && run.fired[0] < 7100);
}

void TestCooldown() {
    std::printf("second splash inside cooldown is blocked\n");
    auto run = Simulate({{2000, 500, 0.5f}, {4500, 500, 0.5f}}, 8000, Fishing());
    CHECK(run.fired.size() == 1);
    bool sawCooldown = false;
    for (auto d : run.decisions) sawCooldown |= d == AlertDecision::BlockedCooldown;
    CHECK(sawCooldown);
}

void TestContinuousAudioFiresOnce() {
    std::printf("continuous loud audio (music) fires once, not repeatedly\n");
    auto run = Simulate({{1000, 20000, 0.4f}}, 25000, Fishing());
    CHECK(run.fired.size() == 1);
}

void TestDipsDoNotRetrigger() {
    std::printf("loud audio with gaps shorter than quiet-before does not re-trigger\n");
    std::vector<Tone> tones;
    for (TimeMs t = 1000; t < 20000; t += 700) tones.push_back({t, 500, 0.4f});  // 200 ms gaps
    AlertParams p = Fishing();
    p.cooldownMs = 0;
    auto run = Simulate(tones, 21000, p);
    CHECK(run.fired.size() == 1);
}

void TestInputSuppression() {
    std::printf("loud sound right after input is suppressed; later splash fires\n");
    AlertParams p = Fishing();
    p.suppressAfterInputMs = 1200;
    // Loud cast 100 ms after a key press, splash 6 s later.
    auto run = Simulate({{1100, 450, 0.5f}, {7000, 800, 0.5f}}, 10000, p, 20, {1000});
    CHECK(run.fired.size() == 1);
    CHECK(!run.fired.empty() && run.fired[0] >= 7000);
    bool sawInput = false;
    for (auto d : run.decisions) sawInput |= d == AlertDecision::BlockedInput;
    CHECK(sawInput);
}

void TestMinLoud() {
    std::printf("must-stay-loud filters clicks\n");
    AlertParams p = Fishing();
    p.minLoudMs = 150;
    auto run = Simulate({{2000, 40, 0.9f}, {6000, 600, 0.5f}}, 9000, p);
    CHECK(run.fired.size() == 1);
    CHECK(!run.fired.empty() && run.fired[0] >= 6150 && run.fired[0] < 6250);
    bool sawShort = false;
    for (auto d : run.decisions) sawShort |= d == AlertDecision::BlockedTooShort;
    CHECK(sawShort);
}

void TestFishingLoopWithJitter() {
    std::printf("randomised fishing loop with timing jitter: one alert per splash\n");
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> wait(3000, 12000);
    std::vector<Tone> tones;
    std::vector<TimeMs> splashes;
    TimeMs t = 1000;
    for (int i = 0; i < 40; ++i) {
        tones.push_back({t, 450, 0.08f});  // cast
        t += wait(rng);
        tones.push_back({t, 800, 0.5f});   // splash
        splashes.push_back(t);
        t += 2500;
    }
    for (unsigned seed : {1u, 2u, 3u}) {
        auto run = Simulate(tones, t + 2000, Fishing(), 20, {}, seed);
        CHECK(run.fired.size() == splashes.size());
        size_t matched = 0;
        for (size_t i = 0; i < splashes.size() && i < run.fired.size(); ++i) {
            if (run.fired[i] >= splashes[i] && run.fired[i] - splashes[i] <= 60) ++matched;
        }
        CHECK(matched == splashes.size());
    }
}

void TestSegmenter() {
    std::printf("sound segmenter start/end\n");
    SoundSegmenter seg;
    int started = 0, ended = 0;
    TimeMs startAt = -1;
    for (TimeMs t = 0; t <= 3000; t += 20) {
        float peak = (t >= 1000 && t < 1500) ? 0.3f : 0.0f;
        auto c = seg.Update(t, peak, 0.001f, 400);
        if (c == SoundSegmenter::Change::Started) {
            ++started;
            startAt = t;
        }
        if (c == SoundSegmenter::Change::Ended) {
            ++ended;
            CHECK(seg.DurationMs() >= 480 && seg.DurationMs() <= 500);
            CHECK(t >= 1880 && t <= 1920);
        }
    }
    CHECK(started == 1);
    CHECK(ended == 1);
    CHECK(startAt == 1000);
}

}  // namespace

int main() {
    TestWildcard();
    TestSplashFiresOnce();
    TestQuietCastIgnored();
    TestCooldown();
    TestContinuousAudioFiresOnce();
    TestDipsDoNotRetrigger();
    TestInputSuppression();
    TestMinLoud();
    TestFishingLoopWithJitter();
    TestSegmenter();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
