// SoundBeacon - a stand-in for a game client, for testing Window Alerts without
// the game. Each instance is its own process with its own window and audio
// session, like one multiboxed WoW client. Press Space (or click) to "cast": it
// plays a quiet cast sound, then a loud "splash" a few seconds later and turns
// orange so you can check the alert border appeared on the right window.
//
//   SoundBeacon.exe [--index=N] [--tile] [--auto] [--cast-volume=0.08]
//                   [--splash-volume=0.5] [--min-wait=3000] [--max-wait=12000]
//                   [--log=beacons.csv] [--exit-after=SECONDS] [--seed=N]
//
// --auto casts by itself (no input), --tile places windows in a grid by index,
// --log appends "unix_ms,local_time,pid,index,event" lines (all instances can
// share one file) so alert timing can be checked against the tracker's log.

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

struct Options {
    int index = 1;
    bool tile = false;
    bool autoCast = false;
    double castVolume = 0.08;
    double splashVolume = 0.5;
    int minWaitMs = 3000;
    int maxWaitMs = 12000;
    std::wstring logPath;
    int exitAfterSeconds = 0;
    unsigned seed = 0;
};

enum class State { Idle, Waiting };

Options g_opt;
State g_state = State::Idle;
ULONGLONG g_splashAt = 0;
ULONGLONG g_nextAutoCast = 0;
ULONGLONG g_flashUntil = 0;
std::wstring g_status = L"Press Space or click to cast";
std::wstring g_lastSplash = L"-";
int g_casts = 0;
int g_splashes = 0;
std::mt19937 g_rng;
std::vector<uint8_t> g_castWav;
std::vector<uint8_t> g_splashWav;
HFONT g_bigFont = nullptr;
HFONT g_font = nullptr;

constexpr int kSampleRate = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<uint8_t> MakeWav(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> wav(44 + samples.size() * 2);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) wav[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    auto put16 = [&](size_t at, uint16_t v) { wav[at] = static_cast<uint8_t>(v); wav[at + 1] = static_cast<uint8_t>(v >> 8); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, static_cast<uint32_t>(wav.size() - 8));
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);  // PCM
    put16(22, 1);  // mono
    put32(24, kSampleRate);
    put32(28, kSampleRate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, static_cast<uint32_t>(samples.size() * 2));
    memcpy(&wav[44], samples.data(), samples.size() * 2);
    return wav;
}

// Soft descending "whoosh" tone.
std::vector<uint8_t> MakeCast(double volume) {
    int n = kSampleRate * 450 / 1000;
    std::vector<int16_t> s(static_cast<size_t>(n));
    double phase = 0;
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / n;
        double freq = 900 - 600 * t;
        phase += 2 * kPi * freq / kSampleRate;
        double env = std::sin(kPi * t);
        s[static_cast<size_t>(i)] = static_cast<int16_t>(32767 * volume * env * std::sin(phase));
    }
    return MakeWav(s);
}

// Noise burst with a fast attack and slow decay, like a splash.
std::vector<uint8_t> MakeSplash(double volume) {
    int n = kSampleRate * 800 / 1000;
    std::vector<int16_t> s(static_cast<size_t>(n));
    std::mt19937 noise(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    double low = 0;
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        double env = (t < 0.01 ? t / 0.01 : 1.0) * std::exp(-t * 4.0);
        low = low * 0.6 + dist(noise) * 0.4;  // Gentle low-pass so it sounds watery
        double v = std::max(-1.0, std::min(1.0, low * 2.2));
        s[static_cast<size_t>(i)] = static_cast<int16_t>(32767 * volume * env * v);
    }
    return MakeWav(s);
}

std::wstring LocalTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void LogEvent(const wchar_t* event) {
    if (g_opt.logPath.empty()) return;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    ULONGLONG unixMs = (t - 116444736000000000ULL) / 10000ULL;
    wchar_t line[256];
    swprintf_s(line, L"%llu,%ls,%lu,%d,%ls\r\n", unixMs, LocalTime().c_str(), GetCurrentProcessId(), g_opt.index, event);
    char narrow[256];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, narrow, sizeof(narrow), nullptr, nullptr);
    HANDLE file = CreateFileW(g_opt.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, narrow, static_cast<DWORD>(len > 0 ? len - 1 : 0), &written, nullptr);
    CloseHandle(file);
}

int RandomBetween(int lo, int hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<int>(lo, hi)(g_rng);
}

void Cast(HWND hwnd) {
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_castWav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    ++g_casts;
    g_state = State::Waiting;
    g_splashAt = GetTickCount64() + static_cast<ULONGLONG>(RandomBetween(g_opt.minWaitMs, g_opt.maxWaitMs));
    g_status = L"Line in the water... waiting for a bite";
    LogEvent(L"cast");
    InvalidateRect(hwnd, nullptr, TRUE);
}

void Splash(HWND hwnd) {
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_splashWav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    ++g_splashes;
    g_state = State::Idle;
    g_lastSplash = LocalTime();
    g_flashUntil = GetTickCount64() + 1500;
    g_status = g_opt.autoCast ? L"SPLASH! (auto mode will cast again)" : L"SPLASH! Press Space or click to cast again";
    if (g_opt.autoCast) g_nextAutoCast = GetTickCount64() + static_cast<ULONGLONG>(RandomBetween(1500, 3000));
    LogEvent(L"splash");
    InvalidateRect(hwnd, nullptr, TRUE);
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    bool flashing = GetTickCount64() < g_flashUntil;
    HBRUSH bg = CreateSolidBrush(flashing ? RGB(255, 160, 40) : RGB(245, 247, 250));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 30, 40));

    HGDIOBJ old = SelectObject(dc, g_bigFont);
    std::wstring title = L"Beacon #" + std::to_wstring(g_opt.index);
    RECT r{16, 12, rc.right - 16, 60};
    DrawTextW(dc, title.c_str(), -1, &r, DT_SINGLELINE);
    SelectObject(dc, g_font);
    std::wstring lines = L"PID " + std::to_wstring(GetCurrentProcessId()) + (g_opt.autoCast ? L"   [auto casting]" : L"") +
                         L"\n\n" + g_status + L"\n\nLast splash: " + g_lastSplash + L"\nCasts: " + std::to_wstring(g_casts) +
                         L"   Splashes: " + std::to_wstring(g_splashes) + L"\n\nSpace/click = cast,  A = toggle auto";
    r = RECT{16, 64, rc.right - 16, rc.bottom - 8};
    DrawTextW(dc, lines.c_str(), -1, &r, DT_WORDBREAK);
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER: {
            ULONGLONG now = GetTickCount64();
            if (wParam == 2) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (g_state == State::Waiting && now >= g_splashAt) Splash(hwnd);
            else if (g_state == State::Idle && g_opt.autoCast && g_nextAutoCast && now >= g_nextAutoCast) {
                g_nextAutoCast = 0;
                Cast(hwnd);
            }
            static bool wasFlashing = false;
            bool flashing = now < g_flashUntil;
            if (flashing != wasFlashing) InvalidateRect(hwnd, nullptr, TRUE);
            wasFlashing = flashing;
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_SPACE && g_state == State::Idle) Cast(hwnd);
            if (wParam == 'A') {
                g_opt.autoCast = !g_opt.autoCast;
                if (g_opt.autoCast && g_state == State::Idle) g_nextAutoCast = GetTickCount64() + 500;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (g_state == State::Idle) Cast(hwnd);
            return 0;
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PlaySoundW(nullptr, nullptr, 0);
            LogEvent(L"exit");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ParseArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        auto value = [&](const wchar_t* prefix) -> const wchar_t* {
            size_t n = wcslen(prefix);
            return a.compare(0, n, prefix) == 0 ? a.c_str() + n : nullptr;
        };
        const wchar_t* v = nullptr;
        if (a == L"--auto") g_opt.autoCast = true;
        else if (a == L"--tile") g_opt.tile = true;
        else if ((v = value(L"--index="))) g_opt.index = _wtoi(v);
        else if ((v = value(L"--cast-volume="))) g_opt.castVolume = _wtof(v);
        else if ((v = value(L"--splash-volume="))) g_opt.splashVolume = _wtof(v);
        else if ((v = value(L"--min-wait="))) g_opt.minWaitMs = _wtoi(v);
        else if ((v = value(L"--max-wait="))) g_opt.maxWaitMs = _wtoi(v);
        else if ((v = value(L"--log="))) g_opt.logPath = v;
        else if ((v = value(L"--exit-after="))) g_opt.exitAfterSeconds = _wtoi(v);
        else if ((v = value(L"--seed="))) g_opt.seed = static_cast<unsigned>(_wtoi(v));
    }
    if (argv) LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    ParseArgs();
    g_rng.seed(g_opt.seed ? g_opt.seed : static_cast<unsigned>(GetCurrentProcessId() ^ GetTickCount()));
    g_castWav = MakeCast(g_opt.castVolume);
    g_splashWav = MakeSplash(g_opt.splashVolume);
    g_bigFont = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hIcon = LoadIcon(nullptr, IDI_INFORMATION);
    wc.lpszClassName = L"SoundBeaconWindow";
    RegisterClassExW(&wc);

    const int w = 440, h = 300;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g_opt.tile) {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int cols = (std::max)(1, static_cast<int>((work.right - work.left) / (w + 10)));
        int i = (std::max)(0, g_opt.index - 1);
        x = work.left + 10 + (i % cols) * (w + 10);
        y = work.top + 10 + (i / cols) * (h + 10);
    }
    std::wstring title = L"Sound Beacon #" + std::to_wstring(g_opt.index) + L" (PID " + std::to_wstring(GetCurrentProcessId()) + L")";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, x, y, w, h,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    SetTimer(hwnd, 1, 10, nullptr);
    if (g_opt.exitAfterSeconds > 0) SetTimer(hwnd, 2, static_cast<UINT>(g_opt.exitAfterSeconds) * 1000, nullptr);
    LogEvent(L"start");
    if (g_opt.autoCast) g_nextAutoCast = GetTickCount64() + static_cast<ULONGLONG>(RandomBetween(500, 2500));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
