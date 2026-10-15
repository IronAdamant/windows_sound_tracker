#pragma once
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AudioEvent.h"
#include "Settings.h"
#include "SoundDetector.h"

// Raised on the monitor thread when an alert rule fires.
struct AlertFire {
    DWORD processId = 0;
    DWORD windowOwnerPid = 0;
    std::wstring processName;
    AlertRule rule;
    float peak = 0.0f;
    TimeMs quietForMs = -1;
    std::chrono::steady_clock::time_point detectedAt;
};

// Snapshot of one audio session for the live levels view.
struct LiveSessionInfo {
    DWORD processId = 0;
    DWORD windowOwnerPid = 0;
    std::wstring processName;
    std::wstring windowTitle;
    std::wstring deviceName;
    bool isSystem = false;
    float peakNow = 0.0f;
    float peakRecentMax = 0.0f;  // Loudest reading in roughly the last 2-3 seconds
    std::wstring ruleNames;
    std::wstring alertState;
    uint64_t alertsFired = 0;
};

// Watches every audio session on every output device.
//
// A dedicated MTA thread owns all Core Audio objects. It samples each session's
// peak meter every few milliseconds, turns the readings into discrete sounds
// (for the list and CSV) and runs the alert rules. Session and device changes
// arrive as notifications, with a periodic re-scan as a safety net.
class SoundTracker {
public:
    using AlertCallback = std::function<void(const AlertFire&)>;

    explicit SoundTracker(std::wstring logsDirectory);
    ~SoundTracker();
    SoundTracker(const SoundTracker&) = delete;
    SoundTracker& operator=(const SoundTracker&) = delete;

    void Start();
    void Stop();
    bool IsRunning() const;

    // Called on the monitor thread; must be quick (post a message and return).
    void SetAlertCallback(AlertCallback callback);
    void SetRules(const std::vector<AlertRule>& rules);
    void SetDetection(const DetectionSettings& detection);
    void SetRecordWindowTitles(bool record);

    // Events with id > afterId, oldest first, at most maxCount (the newest ones).
    std::vector<AudioEvent> GetEventsSnapshot(uint64_t afterId, size_t maxCount) const;
    uint64_t GetEventsVersion() const;  // Changes whenever an event is added or updated
    uint64_t GetLastEventId() const;
    uint64_t GetSoundCount() const;
    uint64_t GetAlertCount() const;
    std::vector<LiveSessionInfo> GetLiveSessions() const;
    std::chrono::system_clock::time_point GetStartTime() const;
    std::wstring GetCurrentLogPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
