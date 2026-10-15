#pragma once
#include <windows.h>
#include <mutex>
#include <string>
#include "AudioEvent.h"

// Writes one CSV row per finished sound to <logs>/sound_log_YYYY-MM-DD_HHMMSS.csv.
class Logger {
public:
    explicit Logger(std::wstring logDirectory);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool Initialize();
    void LogEvent(const AudioEvent& event);
    void Close();
    std::wstring GetCurrentLogPath() const;

private:
    void WriteUtf8(const std::wstring& text);

    std::wstring m_directory;
    std::wstring m_path;
    mutable std::mutex m_mutex;
    HANDLE m_file = INVALID_HANDLE_VALUE;
};
