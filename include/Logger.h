#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <vector>
#include "AudioEvent.h"

enum class LogFormat {
    CSV,
    JSON,
    TEXT
};

class Logger {
private:
    std::wstring m_basePath;
    std::wstring m_currentLogPath;
    std::mutex m_fileMutex;
    std::ofstream m_currentLog;  // Changed to regular ofstream for UTF-8
    
    std::wstring FormatTimestamp(const std::chrono::system_clock::time_point& time);
    std::wstring SanitizeForCSV(const std::wstring& input);
    std::string WideToUTF8(const std::wstring& wide);
    
public:
    Logger(const std::wstring& logDirectory);
    ~Logger();
    
    bool Initialize();
    void LogEvent(const AudioEvent& event);
    void LogRawData(const std::wstring& data);
    
    bool ExportEvents(const std::vector<AudioEvent>& events, 
                     const std::wstring& outputPath,
                     LogFormat format);
    
    void Close();
    
    std::wstring GetCurrentLogPath() const { return m_currentLogPath; }
};