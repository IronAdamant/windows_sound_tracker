#pragma once
#include <windows.h>
#include <string>
#include <vector>

// An "alert rule": when a matching process gets loud after a period of quiet,
// highlight its window. Designed for telling identical windows apart (e.g. five
// multiboxed game clients) but works for any application.
struct AlertRule {
    std::wstring name = L"New alert";
    bool enabled = true;
    std::wstring processPattern;         // e.g. "Wow*.exe" (wildcards, ';' separates alternatives)

    // Detection
    float thresholdPercent = 20.0f;      // Peak level (0-100) that counts as loud
    int quietBeforeMs = 1500;            // Must have been below the threshold this long first
    int minLoudMs = 0;                   // Must stay loud this long before firing
    int cooldownMs = 3000;               // Minimum gap between alerts for the same window
    int suppressAfterInputMs = 0;        // Ignore sounds this soon after keyboard/mouse input (0 = off)
    int suppressScope = 0;               // 0 = only the window that had focus, 1 = all windows

    // Actions
    bool showBorder = true;
    COLORREF borderColor = RGB(255, 40, 40);
    int borderThickness = 12;            // Pixels at 100% scaling
    int flashDurationMs = 2500;
    bool blink = true;
    bool showLabel = true;
    bool flashTaskbar = true;            // Flash the taskbar button until the window is focused

    bool operator==(const AlertRule& o) const;
    bool operator!=(const AlertRule& o) const { return !(*this == o); }
    std::wstring Describe() const;
};

AlertRule MakeWowFishingPreset();

struct DetectionSettings {
    float soundThreshold = 0.001f;  // Peak level that counts as "a sound is playing" for the log
    int soundReleaseMs = 400;       // Silence needed before a sound counts as finished
    int meterIntervalMs = 20;       // How often audio levels are sampled
};

struct Settings {
    bool startTrackingOnLaunch = false;
    int debugPreset = 0;  // dbg::Preset
    bool recordWindowTitles = true;
    DetectionSettings detection;

    bool hasWindowRect = false;
    RECT windowRect{};
    bool filterEnabled = false;
    std::wstring filterText;

    std::vector<AlertRule> alerts;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    std::wstring Describe() const;
};

// Where settings and logs live. "Portable" (next to the exe) when that folder is
// writable, otherwise %LOCALAPPDATA%\WindowsSoundTracker.
namespace AppPaths {
void Init(const std::wstring& overrideDataDir);
const std::wstring& ExeDir();
const std::wstring& ExePath();
const std::wstring& DataDir();
const std::wstring& LogsDir();
const std::wstring& SettingsFile();
bool IsPortable();
}  // namespace AppPaths
