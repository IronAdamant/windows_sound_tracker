#include "Logger.h"
#include "DebugLog.h"

#include <iomanip>
#include <sstream>

namespace {

std::wstring FormatTimestamp(const std::chrono::system_clock::time_point& time) {
    auto t = std::chrono::system_clock::to_time_t(time);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    std::tm tm = {};
    localtime_s(&tm, &t);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
               tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::wstring Csv(const std::wstring& input) {
    if (input.find_first_of(L",\"\n\r") == std::wstring::npos) return input;
    std::wstring out = L"\"";
    for (wchar_t c : input) {
        if (c == L'"') out += L'"';
        out += c;
    }
    out += L'"';
    return out;
}

}  // namespace

Logger::Logger(std::wstring logDirectory) : m_directory(std::move(logDirectory)) {}

Logger::~Logger() { Close(); }

bool Logger::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    CreateDirectoryW(m_directory.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\sound_log_%04u-%02u-%02u_%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    m_path = m_directory + name;
    m_file = CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == INVALID_HANDLE_VALUE) {
        LOG_ERROR("csv", L"Cannot create sound log " << m_path << L": " << dbg::Win32ErrorToString(GetLastError()));
        return false;
    }
    static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};  // So Excel opens it as UTF-8
    DWORD written = 0;
    WriteFile(m_file, bom, sizeof(bom), &written, nullptr);
    WriteUtf8(L"StartTime,DurationMs,ProcessID,ProcessName,ProcessPath,Description,SessionName,WindowTitle,"
              L"OutputDevice,VolumeLevel,PeakLevel,IsSystemSound,DeviceChange,AlertsFired\r\n");
    LOG_INFO("csv", L"Sound log: " << m_path);
    return true;
}

void Logger::LogEvent(const AudioEvent& e) {
    std::wostringstream line;
    line << FormatTimestamp(e.timestamp) << L"," << e.durationMs << L"," << e.processId << L"," << Csv(e.processName)
         << L"," << Csv(e.processPath) << L"," << Csv(e.soundDescription) << L"," << Csv(e.sessionDisplayName) << L","
         << Csv(e.windowTitle) << L"," << Csv(e.deviceName) << L"," << std::fixed << std::setprecision(1)
         << (e.volumeLevel * 100) << L"%," << (e.peakLevel * 100) << L"%," << (e.isSystemSound ? L"Yes" : L"No") << L","
         << Csv(e.deviceChangeInfo) << L"," << e.alertsFired << L"\r\n";
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteUtf8(line.str());
}

void Logger::WriteUtf8(const std::wstring& text) {
    if (m_file == INVALID_HANDLE_VALUE || text.empty()) return;
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &utf8[0], size, nullptr, nullptr);
    DWORD written = 0;
    if (!WriteFile(m_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr)) {
        LOG_WARN("csv", L"Write to sound log failed: " << dbg::Win32ErrorToString(GetLastError()));
    }
}

void Logger::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file != INVALID_HANDLE_VALUE) {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
}

std::wstring Logger::GetCurrentLogPath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}
