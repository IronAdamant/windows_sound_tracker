#include "DebugLog.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace dbg {

std::atomic<int> g_level{static_cast<int>(Level::Info)};

namespace {

constexpr size_t kMaxQueuedLines = 50000;
constexpr unsigned long long kMaxFileBytes = 20ull * 1024 * 1024;
constexpr size_t kKeepLogFiles = 10;
constexpr size_t kKeepDumpFiles = 3;

struct State {
    std::mutex queueMutex;
    std::condition_variable cv;
    std::deque<std::string> queue;
    size_t dropped = 0;
    bool stop = false;
    std::thread writer;

    // Guards the file handle. The writer takes lines off the queue only while
    // holding it, so a crash handler that holds it sees every unwritten line.
    std::timed_mutex fileMutex;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring directory;
    std::wstring basePath;  // Without extension
    std::wstring currentPath;
    unsigned long long bytes = 0;
    int part = 1;

    std::wstring dumpDirectory;
    LARGE_INTEGER qpcStart{};
    LARGE_INTEGER qpcFreq{};
};

// Intentionally leaked so logging keeps working during static destruction.
State& S() {
    static State* state = [] {
        auto* s = new State();
        QueryPerformanceFrequency(&s->qpcFreq);
        QueryPerformanceCounter(&s->qpcStart);
        return s;
    }();
    return *state;
}

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &utf8[0], size, nullptr, nullptr);
    return utf8;
}

std::wstring FileTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

const wchar_t* LevelTag(Level level) {
    switch (level) {
        case Level::Error: return L"ERROR";
        case Level::Warn:  return L"WARN ";
        case Level::Info:  return L"INFO ";
        case Level::Debug: return L"DEBUG";
        case Level::Trace: return L"TRACE";
    }
    return L"?    ";
}

// Deletes all but the newest 'keep' files matching pattern (names sort by timestamp).
void PruneFiles(const std::wstring& directory, const wchar_t* pattern, size_t keep) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((directory + L"\\" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (names.size() <= keep) return;
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + keep < names.size(); ++i) {
        DeleteFileW((directory + L"\\" + names[i]).c_str());
    }
}

// Caller holds fileMutex.
bool OpenFileLocked(State& s, const std::wstring& path) {
    if (s.file != INVALID_HANDLE_VALUE) CloseHandle(s.file);
    s.file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    s.bytes = 0;
    if (s.file == INVALID_HANDLE_VALUE) return false;
    s.currentPath = path;
    static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(s.file, bom, sizeof(bom), &written, nullptr);
    return true;
}

// Caller holds fileMutex.
void WriteRawLocked(State& s, const std::string& line) {
    if (s.file == INVALID_HANDLE_VALUE) return;
    if (s.bytes + line.size() > kMaxFileBytes) {
        ++s.part;
        std::wstring next = s.basePath + L"_part" + std::to_wstring(s.part) + L".log";
        std::string notice = "--- log continues in " + ToUtf8(next) + " ---\r\n";
        DWORD w = 0;
        WriteFile(s.file, notice.data(), static_cast<DWORD>(notice.size()), &w, nullptr);
        if (!OpenFileLocked(s, next)) return;
    }
    DWORD written = 0;
    WriteFile(s.file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    s.bytes += written;
}

std::string FormatLine(Level level, const char* category, const std::wstring& message) {
    State& s = S();
    SYSTEMTIME st;
    GetLocalTime(&st);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = static_cast<double>(now.QuadPart - s.qpcStart.QuadPart) / static_cast<double>(s.qpcFreq.QuadPart);

    wchar_t cat[16] = {};
    for (int i = 0; i < 15 && category && category[i]; ++i) cat[i] = static_cast<wchar_t>(category[i]);

    wchar_t prefix[160];
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u +%10.3fs [%ls] [%5lu] [%-8ls] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               elapsed, LevelTag(level), GetCurrentThreadId(), cat);
    std::wstring line = prefix;
    line += message;
    line += L"\r\n";
    OutputDebugStringW(line.c_str());
    return ToUtf8(line);
}

void WriterLoop() {
    State& s = S();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s.queueMutex);
            s.cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return s.stop || !s.queue.empty(); });
            if (s.stop && s.queue.empty()) break;
        }
        std::lock_guard<std::timed_mutex> fileLock(s.fileMutex);
        std::deque<std::string> batch;
        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            batch.swap(s.queue);
            dropped = s.dropped;
            s.dropped = 0;
        }
        if (dropped) {
            WriteRawLocked(s, FormatLine(Level::Warn, "log", L"Log queue overflow: " + std::to_wstring(dropped) + L" lines dropped"));
        }
        for (const auto& line : batch) WriteRawLocked(s, line);
    }
}

// Pointers in MINIDUMP_EXCEPTION_INFORMATION are 4-byte packed, as in dbghelp.h.
#pragma pack(push, 4)
struct MiniDumpExceptionInfo {
    DWORD ThreadId;
    PEXCEPTION_POINTERS ExceptionPointers;
    BOOL ClientPointers;
};
#pragma pack(pop)
typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

void WriteMiniDump(EXCEPTION_POINTERS* ep) {
    State& s = S();
    if (s.dumpDirectory.empty()) return;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (!writeDump) return;
    std::wstring path = s.dumpDirectory + L"\\crash_" + FileTimestamp() + L".dmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MiniDumpExceptionInfo info{GetCurrentThreadId(), ep, FALSE};
    const int dumpType = 0x00000000 /*Normal*/ | 0x00001000 /*WithThreadInfo*/ | 0x00000020 /*WithUnloadedModules*/;
    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, dumpType, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    Write(Level::Error, "crash", ok ? L"Minidump written: " + path : L"Minidump failed: " + Win32ErrorToString(GetLastError()));
    FlushNow();
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> inCrash{false};
    if (inCrash.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* address = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE module = nullptr;
    wchar_t moduleName[MAX_PATH] = L"?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    if (module) GetModuleFileNameW(module, moduleName, MAX_PATH);
    wchar_t msg[512];
    swprintf_s(msg, L"FATAL: unhandled exception 0x%08lX at %p (%ls+0x%llX)", code, address, moduleName,
               static_cast<unsigned long long>(reinterpret_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    Write(Level::Error, "crash", msg);
    FlushNow();
    WriteMiniDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void TerminateHandler() {
    std::wstring what = L"unknown";
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            std::string narrow = e.what();
            what = std::wstring(narrow.begin(), narrow.end());
        } catch (...) {
            what = L"non-std exception";
        }
    }
    Write(Level::Error, "crash", L"FATAL: std::terminate called (" + what + L")");
    FlushNow();
    std::abort();
}

}  // namespace

Level LevelForPreset(Preset preset) {
    switch (preset) {
        case Preset::Debug: return Level::Debug;
        case Preset::Verbose: return Level::Trace;
        default: return Level::Info;
    }
}

const wchar_t* PresetName(Preset preset) {
    switch (preset) {
        case Preset::Debug: return L"Debug";
        case Preset::Verbose: return L"Verbose";
        default: return L"Normal";
    }
}

bool Init(const std::wstring& logDirectory, Level level) {
    State& s = S();
    SetLevel(level);
    s.directory = logDirectory;
    CreateDirectoryW(logDirectory.c_str(), nullptr);
    PruneFiles(logDirectory, L"debug_*.log", kKeepLogFiles - 1);
    s.basePath = logDirectory + L"\\debug_" + FileTimestamp();
    bool ok;
    {
        std::lock_guard<std::timed_mutex> fileLock(s.fileMutex);
        ok = OpenFileLocked(s, s.basePath + L".log");
    }
    if (!s.writer.joinable()) s.writer = std::thread(WriterLoop);
    return ok;
}

void Shutdown() {
    State& s = S();
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        s.stop = true;
    }
    s.cv.notify_all();
    if (s.writer.joinable()) s.writer.join();
    std::lock_guard<std::timed_mutex> fileLock(s.fileMutex);
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
}

void SetLevel(Level level) { g_level.store(static_cast<int>(level)); }
Level GetLevel() { return static_cast<Level>(g_level.load()); }

void Write(Level level, const char* category, const std::wstring& message) {
    if (!Enabled(level)) return;
    std::string line = FormatLine(level, category, message);
    State& s = S();
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        if (s.stop) return;
        if (s.queue.size() >= kMaxQueuedLines) {
            ++s.dropped;
            return;
        }
        s.queue.push_back(std::move(line));
    }
    if (level <= Level::Warn) s.cv.notify_one();
}

void FlushNow() {
    State& s = S();
    // Timed locks: a crash may have happened while another thread held a lock.
    // Waiting on the file lock lets the writer finish a batch it already took.
    bool fileLocked = s.fileMutex.try_lock_for(std::chrono::milliseconds(500));
    std::deque<std::string> batch;
    for (int i = 0; i < 50; ++i) {
        if (s.queueMutex.try_lock()) {
            batch.swap(s.queue);
            s.queueMutex.unlock();
            break;
        }
        Sleep(2);
    }
    for (const auto& line : batch) WriteRawLocked(s, line);
    if (s.file != INVALID_HANDLE_VALUE) FlushFileBuffers(s.file);
    if (fileLocked) s.fileMutex.unlock();
}

std::wstring CurrentLogPath() {
    State& s = S();
    std::lock_guard<std::timed_mutex> fileLock(s.fileMutex);
    return s.currentPath;
}

void InstallCrashHandlers(const std::wstring& dumpDirectory) {
    S().dumpDirectory = dumpDirectory;
    PruneFiles(dumpDirectory, L"crash_*.dmp", kKeepDumpFiles);
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(TerminateHandler);
}

std::wstring Win32ErrorToString(DWORD error) {
    wchar_t* text = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message;
    if (len && text) {
        message.assign(text, len);
        LocalFree(text);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ' || message.back() == L'.')) {
            message.pop_back();
        }
    }
    wchar_t code[32];
    swprintf_s(code, L"%lu", error);
    return message.empty() ? std::wstring(L"error ") + code : std::wstring(L"error ") + code + L": " + message;
}

std::wstring HrToString(HRESULT hr) {
    // Core Audio codes that FormatMessage does not know.
    struct Known { HRESULT hr; const wchar_t* name; };
    static const Known known[] = {
        {static_cast<HRESULT>(0x88890001L), L"AUDCLNT_E_NOT_INITIALIZED"},
        {static_cast<HRESULT>(0x88890004L), L"AUDCLNT_E_DEVICE_INVALIDATED"},
        {static_cast<HRESULT>(0x8889000AL), L"AUDCLNT_E_DEVICE_IN_USE"},
        {static_cast<HRESULT>(0x88890010L), L"AUDCLNT_E_SERVICE_NOT_RUNNING"},
        {static_cast<HRESULT>(0x88890008L), L"AUDCLNT_E_UNSUPPORTED_FORMAT"},
        {static_cast<HRESULT>(0x0889000DL), L"AUDCLNT_S_NO_SINGLE_PROCESS"},
    };
    wchar_t code[16];
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
    for (const auto& k : known) {
        if (k.hr == hr) return std::wstring(code) + L" (" + k.name + L")";
    }
    wchar_t* text = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message;
    if (len && text) {
        message.assign(text, len);
        LocalFree(text);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ' || message.back() == L'.')) {
            message.pop_back();
        }
    }
    return message.empty() ? std::wstring(code) : std::wstring(code) + L" (" + message + L")";
}

std::wstring Pct(float level) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", level * 100.0f);
    return buf;
}

}  // namespace dbg
