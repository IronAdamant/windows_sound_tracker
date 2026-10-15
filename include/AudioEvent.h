#pragma once
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <string>

// One detected sound: from the moment a session's peak level rises above the
// detection threshold until it has stayed below it for the release time.
struct AudioEvent {
    uint64_t id = 0;                                  // Monotonic id, unique per tracking session
    std::chrono::system_clock::time_point timestamp;  // When the sound started
    DWORD durationMs = 0;                             // How long it stayed audible
    bool ongoing = true;                              // Still playing
    DWORD processId = 0;                              // Process that owns the audio session
    std::wstring processName;                         // Executable name (e.g. "Discord.exe")
    std::wstring processPath;                         // Full path to executable
    std::wstring soundDescription;                    // Human-readable description
    std::wstring sessionDisplayName;                  // Audio session display name
    std::wstring windowTitle;                         // Window(s) of the owning process, if any
    std::wstring deviceName;                          // Output device the session plays on
    std::wstring deviceChangeInfo;                    // Recent device plug/unplug (system sounds only)
    float volumeLevel = 0.0f;                         // Session volume slider (0.0 - 1.0)
    float peakLevel = 0.0f;                           // Loudest meter reading during the sound (0.0 - 1.0)
    bool isSystemSound = false;                       // Windows system sounds session
    DWORD alertsFired = 0;                            // Alerts triggered by this sound
};
