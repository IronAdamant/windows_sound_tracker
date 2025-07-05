#pragma once
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <string>
#include <chrono>

// Separate header for AudioEvent to avoid circular dependencies
// This struct represents a single audio event captured by the tracker
struct AudioEvent {
    std::chrono::system_clock::time_point timestamp;  // When the sound occurred
    DWORD processId = 0;                              // Process ID making the sound
    std::wstring processName;                         // Executable name (e.g., "Discord.exe")
    std::wstring processPath;                         // Full path to executable
    std::wstring soundDescription;                    // Human-readable description
    std::wstring sessionDisplayName;                  // Audio session display name
    float volumeLevel = 0.0f;                         // Current volume (0.0 - 1.0)
    float peakLevel = 0.0f;                           // Peak audio level (0.0 - 1.0)
    bool isSystemSound = false;                       // True if Windows system sound
    DWORD duration_ms = 0;                            // Duration in milliseconds
    DWORD eventCount = 1;                             // Number of events batched (same millisecond)
    std::wstring usbDeviceInfo;                       // USB device information if applicable
    std::wstring browserTabInfo;                      // Browser tab title if applicable
};