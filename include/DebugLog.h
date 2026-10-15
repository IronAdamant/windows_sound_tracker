#pragma once
#include <windows.h>
#include <atomic>
#include <sstream>
#include <string>

// Diagnostic log for dogfooding and remote bug reports.
//
// Lines are formatted on the calling thread and written by a background thread,
// so logging never blocks the audio meter loop on disk I/O. Files go to
// <logs>/debug_YYYY-MM-DD_HHMMSS.log, roll over at 20 MB, and only the newest
// 10 are kept. Unhandled exceptions are logged and a minidump is written.
namespace dbg {

enum class Level : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

// User-facing presets (stored in settings as 0/1/2).
enum class Preset : int { Normal = 0, Debug = 1, Verbose = 2 };
Level LevelForPreset(Preset preset);
const wchar_t* PresetName(Preset preset);

extern std::atomic<int> g_level;
inline bool Enabled(Level level) { return static_cast<int>(level) <= g_level.load(std::memory_order_relaxed); }

bool Init(const std::wstring& logDirectory, Level level);
void Shutdown();
void SetLevel(Level level);
Level GetLevel();
void Write(Level level, const char* category, const std::wstring& message);
void FlushNow();  // Synchronous; used before exits and in crash handlers
std::wstring CurrentLogPath();
void InstallCrashHandlers(const std::wstring& dumpDirectory);

std::wstring HrToString(HRESULT hr);
std::wstring Win32ErrorToString(DWORD error);
std::wstring Pct(float level);  // 0.1234f -> "12.3%"

}  // namespace dbg

#define DBG_LOG(lvl, cat, expr)                         \
    do {                                                \
        if (::dbg::Enabled(lvl)) {                      \
            std::wostringstream dbg_oss_;               \
            dbg_oss_ << expr;                           \
            ::dbg::Write((lvl), (cat), dbg_oss_.str()); \
        }                                               \
    } while (0)

#define LOG_ERROR(cat, expr) DBG_LOG(::dbg::Level::Error, cat, expr)
#define LOG_WARN(cat, expr)  DBG_LOG(::dbg::Level::Warn, cat, expr)
#define LOG_INFO(cat, expr)  DBG_LOG(::dbg::Level::Info, cat, expr)
#define LOG_DEBUG(cat, expr) DBG_LOG(::dbg::Level::Debug, cat, expr)
#define LOG_TRACE(cat, expr) DBG_LOG(::dbg::Level::Trace, cat, expr)
