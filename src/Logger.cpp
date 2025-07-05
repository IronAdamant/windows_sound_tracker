#include "../include/Logger.h"
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

Logger::Logger(const std::wstring& logDirectory) : m_basePath(logDirectory) {
}

Logger::~Logger() {
    Close();
}

bool Logger::Initialize() {
    // Create log directory if it doesn't exist
    CreateDirectoryW(m_basePath.c_str(), NULL);
    
    // Create log filename with timestamp
    auto now = std::chrono::system_clock::now();
    std::wstring filename = m_basePath + L"\\sound_log_" + FormatTimestamp(now) + L".csv";
    filename.erase(std::remove(filename.begin(), filename.end(), L':'), filename.end());
    filename.erase(std::remove(filename.begin(), filename.end(), L' '), filename.end());
    
    // Store filename for status display
    m_currentLogPath = filename;
    
    // Convert wide filename to narrow for ofstream
    std::string narrowFilename = WideToUTF8(filename);
    
    // Open file for UTF-8 output
    m_currentLog.open(narrowFilename, std::ios::out | std::ios::binary);
    if (!m_currentLog.is_open()) {
        return false;
    }
    
    // Write BOM for UTF-8
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    m_currentLog.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    
    // Write CSV header
    std::string header = "Timestamp,EventCount,ProcessID,ProcessName,ProcessPath,Description,SessionName,VolumeLevel,PeakLevel,IsSystemSound\n";
    m_currentLog << header;
    m_currentLog.flush();
    
    return true;
}

std::wstring Logger::FormatTimestamp(const std::chrono::system_clock::time_point& time) {
    auto time_t = std::chrono::system_clock::to_time_t(time);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()) % 1000;
    
    std::tm tm = {};
    if (localtime_s(&tm, &time_t) != 0) {
        // Fallback to current time if conversion fails
        auto now = std::chrono::system_clock::now();
        time_t = std::chrono::system_clock::to_time_t(now);
        localtime_s(&tm, &time_t);
    }
    
    std::wostringstream oss;
    oss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    oss << L"." << std::setfill(L'0') << std::setw(3) << ms.count();
    
    return oss.str();
}

std::wstring Logger::SanitizeForCSV(const std::wstring& input) {
    std::wstring output = input;
    
    // If contains comma, quote, or newline, wrap in quotes and escape quotes
    if (output.find_first_of(L",\"\n\r") != std::wstring::npos) {
        // Escape quotes
        size_t pos = 0;
        while ((pos = output.find(L"\"", pos)) != std::wstring::npos) {
            output.replace(pos, 1, L"\"\"");
            pos += 2;
        }
        // Wrap in quotes
        output = L"\"" + output + L"\"";
    }
    
    return output;
}

void Logger::LogEvent(const AudioEvent& event) {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (!m_currentLog.is_open()) {
        Initialize();
    }
    
    // Build the CSV line as a wide string first
    std::wostringstream line;
    line << FormatTimestamp(event.timestamp) << L","
         << (event.eventCount > 0 ? event.eventCount : 1) << L","
         << event.processId << L","
         << SanitizeForCSV(event.processName) << L","
         << SanitizeForCSV(event.processPath) << L","
         << SanitizeForCSV(event.soundDescription) << L","
         << SanitizeForCSV(event.sessionDisplayName) << L","
         << std::fixed << std::setprecision(2) << (event.volumeLevel * 100) << L"%,"
         << std::fixed << std::setprecision(2) << (event.peakLevel * 100) << L"%,"
         << (event.isSystemSound ? L"Yes" : L"No") << L"\n";
    
    // Convert to UTF-8 and write
    std::string utf8Line = WideToUTF8(line.str());
    m_currentLog << utf8Line;
    m_currentLog.flush();
}

void Logger::LogRawData(const std::wstring& data) {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (m_currentLog.is_open()) {
        std::string utf8Data = WideToUTF8(data + L"\n");
        m_currentLog << utf8Data;
        m_currentLog.flush();
    }
}

bool Logger::ExportEvents(const std::vector<AudioEvent>& events, 
                         const std::wstring& outputPath,
                         LogFormat format) {
    std::wofstream output(outputPath);
    if (!output.is_open()) {
        return false;
    }
    
    switch (format) {
        case LogFormat::CSV: {
            // Write header
            output << L"Timestamp,EventCount,ProcessID,ProcessName,ProcessPath,Description,SessionName,VolumeLevel,PeakLevel,IsSystemSound,Duration(ms)\n";
            
            // Calculate durations
            for (size_t i = 0; i < events.size(); ++i) {
                const auto& event = events[i];
                DWORD duration = 0;
                
                // Find duration by looking for the next different process
                if (i + 1 < events.size()) {
                    for (size_t j = i + 1; j < events.size(); ++j) {
                        if (events[j].processId != event.processId) {
                            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                                events[j-1].timestamp - event.timestamp);
                            duration = static_cast<DWORD>(diff.count());
                            break;
                        }
                    }
                }
                
                output << FormatTimestamp(event.timestamp) << L","
                      << (event.eventCount > 0 ? event.eventCount : 1) << L","
                      << event.processId << L","
                      << SanitizeForCSV(event.processName) << L","
                      << SanitizeForCSV(event.processPath) << L","
                      << SanitizeForCSV(event.soundDescription) << L","
                      << SanitizeForCSV(event.sessionDisplayName) << L","
                      << std::fixed << std::setprecision(2) << (event.volumeLevel * 100) << L"%,"
                      << std::fixed << std::setprecision(2) << (event.peakLevel * 100) << L"%,"
                      << (event.isSystemSound ? L"Yes" : L"No") << L","
                      << duration << L"\n";
            }
            break;
        }
        
        case LogFormat::JSON: {
            output << L"{\n  \"events\": [\n";
            
            for (size_t i = 0; i < events.size(); ++i) {
                const auto& event = events[i];
                output << L"    {\n"
                      << L"      \"timestamp\": \"" << FormatTimestamp(event.timestamp) << L"\",\n"
                      << L"      \"eventCount\": " << (event.eventCount > 0 ? event.eventCount : 1) << L",\n"
                      << L"      \"processId\": " << event.processId << L",\n"
                      << L"      \"processName\": \"" << event.processName << L"\",\n"
                      << L"      \"processPath\": \"" << event.processPath << L"\",\n"
                      << L"      \"description\": \"" << event.soundDescription << L"\",\n"
                      << L"      \"volumeLevel\": " << (event.volumeLevel * 100) << L",\n"
                      << L"      \"peakLevel\": " << (event.peakLevel * 100) << L",\n"
                      << L"      \"isSystemSound\": " << (event.isSystemSound ? L"true" : L"false") << L"\n"
                      << L"    }";
                
                if (i < events.size() - 1) {
                    output << L",";
                }
                output << L"\n";
            }
            
            output << L"  ]\n}\n";
            break;
        }
        
        case LogFormat::TEXT: {
            output << L"Windows Sound Tracker Log\n";
            output << L"=========================\n\n";
            
            for (const auto& event : events) {
                output << L"Time: " << FormatTimestamp(event.timestamp);
                if (event.eventCount > 1) {
                    output << L" (" << event.eventCount << L" events)";
                }
                output << L"\n"
                      << L"Process: " << event.processName << L" (PID: " << event.processId << L")\n"
                      << L"Path: " << event.processPath << L"\n"
                      << L"Description: " << event.soundDescription << L"\n"
                      << L"Volume: " << std::fixed << std::setprecision(1) << (event.volumeLevel * 100) << L"%"
                      << L" | Peak: " << (event.peakLevel * 100) << L"%\n"
                      << L"System Sound: " << (event.isSystemSound ? L"Yes" : L"No") << L"\n"
                      << L"---\n\n";
            }
            break;
        }
    }
    
    output.close();
    return true;
}

void Logger::Close() {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    if (m_currentLog.is_open()) {
        m_currentLog.close();
    }
}

std::string Logger::WideToUTF8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    
    // Get required buffer size
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    
    // Convert
    std::string utf8(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    
    return utf8;
}