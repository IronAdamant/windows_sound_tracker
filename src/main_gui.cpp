#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

#include "DebugLog.h"
#include "Settings.h"
#include "SoundTrackerGUI.h"
#include "Version.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

struct CommandLine {
    bool debug = false;
    bool verbose = false;
    bool start = false;
    bool help = false;
    bool testCrash = false;
    int exitAfter = 0;
    std::wstring dataDir;
    std::wstring raw;
};

CommandLine ParseCommandLine() {
    CommandLine cl;
    cl.raw = GetCommandLineW();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--debug") cl.debug = true;
        else if (a == L"--verbose") cl.verbose = true;
        else if (a == L"--start") cl.start = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?") cl.help = true;
        else if (a == L"--test-crash") cl.testCrash = true;
        else if (a.rfind(L"--exit-after=", 0) == 0) cl.exitAfter = _wtoi(a.c_str() + 13);
        else if (a.rfind(L"--data-dir=", 0) == 0) cl.dataDir = a.substr(11);
    }
    if (argv) LocalFree(argv);
    return cl;
}

std::wstring Widen(const char* s) {
    std::wstring out;
    while (s && *s) out += static_cast<wchar_t>(static_cast<unsigned char>(*s++));
    return out;
}

std::wstring CompilerName() {
#if defined(__clang__)
    return L"clang " + Widen(__clang_version__);
#elif defined(_MSC_VER)
    return L"MSVC " + std::to_wstring(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return L"gcc " + Widen(__VERSION__);
#else
    return L"unknown compiler";
#endif
}

std::wstring RegString(const wchar_t* name) {
    wchar_t buf[128] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        return buf;
    }
    return std::wstring();
}

std::wstring OsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (fn) fn(&vi);
    DWORD ubr = 0, size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    std::wstring product = RegString(L"ProductName");
    // Windows 11 still reports "Windows 10" in ProductName; build 22000+ is Windows 11.
    if (vi.dwBuildNumber >= 22000 && product.rfind(L"Windows 10", 0) == 0) product.replace(0, 10, L"Windows 11");
    return product + L" " + RegString(L"DisplayVersion") + L" (" + std::to_wstring(vi.dwMajorVersion) + L"." +
           std::to_wstring(vi.dwMinorVersion) + L"." + std::to_wstring(vi.dwBuildNumber) + L"." + std::to_wstring(ubr) + L")";
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

void LogMonitors() {
    using SetCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    auto setCtx = reinterpret_cast<SetCtxFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    auto getDpi = shcore ? reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"))) : nullptr;
    DPI_AWARENESS_CONTEXT previous = setCtx ? setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    struct Ctx { GetDpiForMonitorFn getDpi; } ctx{getDpi};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(monitor, &mi);
            UINT dpiX = 96, dpiY = 96;
            if (c->getDpi) c->getDpi(monitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
            const RECT& r = mi.rcMonitor;
            LOG_INFO("system", L"Display " << mi.szDevice << L": " << r.left << L"," << r.top << L" " << (r.right - r.left) << L"x"
                                           << (r.bottom - r.top) << L" px, scale " << (dpiX * 100 / 96) << L"%"
                                           << ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (primary)" : L""));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (setCtx && previous) setCtx(previous);
    if (shcore) FreeLibrary(shcore);
}

void LogBanner(const CommandLine& cl, const Settings& settings, bool settingsLoaded) {
    LOG_INFO("system", L"===== " << APP_NAME_WSTR << L" " << APP_VERSION_WSTR << L" (built " << Widen(__DATE__) << L" "
                                 << Widen(__TIME__) << L", " << CompilerName() << L", "
                                 << (sizeof(void*) == 8 ? L"x64" : L"x86") << L") =====");
    LOG_INFO("system", L"OS: " << OsVersion() << L", elevated: " << (IsElevated() ? L"yes" : L"no") << L", PID "
                               << GetCurrentProcessId());
    LOG_INFO("system", L"Command line: " << cl.raw);
    LOG_INFO("system", L"Exe: " << AppPaths::ExePath());
    LOG_INFO("system", L"Data folder: " << AppPaths::DataDir() << (AppPaths::IsPortable() ? L" (portable)" : L" (per-user)"));
    LOG_INFO("system", L"Settings: " << AppPaths::SettingsFile() << (settingsLoaded ? L"" : L" (not found, using defaults)"));
    LOG_INFO("system", L"Debug log level: " << static_cast<int>(dbg::GetLevel()) << L" (0=error .. 4=trace)");
    LogMonitors();
    LOG_INFO("system", L"Settings: " << settings.Describe());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    CommandLine cl = ParseCommandLine();
    if (cl.help) {
        MessageBoxW(nullptr,
                    L"SoundTracker.exe [options]\n\n"
                    L"--start\t\tStart tracking immediately\n"
                    L"--debug\t\tDebug log level for this run\n"
                    L"--verbose\tVerbose log level for this run\n"
                    L"--data-dir=PATH\tUse PATH for settings and logs\n"
                    L"--exit-after=N\tExit after N seconds (testing)\n"
                    L"--test-crash\tCrash on purpose to check crash logs",
                    L"Sound Tracker", MB_ICONINFORMATION);
        return 0;
    }

    AppPaths::Init(cl.dataDir);
    Settings settings;
    bool settingsLoaded = settings.Load(AppPaths::SettingsFile());
    int preset = settings.debugPreset;
    if (cl.debug && preset < 1) preset = 1;
    if (cl.verbose) preset = 2;
    if (!dbg::Init(AppPaths::LogsDir(), dbg::LevelForPreset(static_cast<dbg::Preset>(preset)))) {
        MessageBoxW(nullptr, (L"Cannot create the debug log in " + AppPaths::LogsDir() + L". Continuing without it.").c_str(),
                    L"Sound Tracker", MB_ICONWARNING);
    }
    dbg::InstallCrashHandlers(AppPaths::LogsDir());
    LogBanner(cl, settings, settingsLoaded);
    if (cl.testCrash) {
        // Verifies crash logging and minidumps on a user's machine.
        LOG_WARN("system", L"--test-crash: crashing deliberately");
        volatile int* null = nullptr;
        *null = 1;
    }

    // One instance per data folder: two trackers sharing settings would flash every
    // alert twice. A separate --data-dir (e.g. a test run) may run alongside.
    std::wstring dataDirKey = AppPaths::DataDir();
    CharLowerBuffW(&dataDirKey[0], static_cast<DWORD>(dataDirKey.size()));
    std::wstring mutexName = L"Local\\IronAdamant.WindowsSoundTracker." + std::to_wstring(std::hash<std::wstring>{}(dataDirKey));
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_INFO("system", L"Another instance is already running; activating it and exiting");
        if (HWND existing = FindWindowW(L"SoundTrackerWindow", nullptr)) {
            PostMessageW(existing, WM_COMMAND, ID_MENU_RESTORE, 0);
            SetForegroundWindow(existing);
        }
        dbg::Shutdown();
        CloseHandle(instanceMutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("system", L"CoInitializeEx failed: " << dbg::HrToString(hr));
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Sound Tracker", MB_ICONERROR);
        dbg::Shutdown();
        return 1;
    }

    int result = 1;
    {
        SoundTrackerGUI app(settings);
        LaunchOptions options;
        options.startTracking = cl.start;
        options.exitAfterSeconds = cl.exitAfter;
        if (app.Initialize(hInstance, options)) {
            result = app.Run();
        } else {
            MessageBoxW(nullptr, L"Failed to initialize the application. See the debug log in the logs folder.", L"Sound Tracker", MB_ICONERROR);
        }
    }
    LOG_INFO("system", L"Exiting (code " << result << L")");
    CoUninitialize();
    dbg::Shutdown();
    if (instanceMutex) CloseHandle(instanceMutex);
    return result;
}
