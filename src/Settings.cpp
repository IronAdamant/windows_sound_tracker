#include "Settings.h"

#include <shlobj.h>
#include <algorithm>
#include <cwchar>
#include <sstream>

namespace {

const wchar_t* kGeneral = L"General";

std::wstring ReadStr(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& def) {
    std::wstring buf(2048, L'\0');
    DWORD n = GetPrivateProfileStringW(section, key, def.c_str(), &buf[0], static_cast<DWORD>(buf.size()), path.c_str());
    buf.resize(n);
    return buf;
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def, int lo, int hi) {
    std::wstring s = ReadStr(path, section, key, L"");
    if (s.empty()) return def;
    int v = _wtoi(s.c_str());
    return (std::max)(lo, (std::min)(hi, v));
}

float ReadFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float def, float lo, float hi) {
    std::wstring s = ReadStr(path, section, key, L"");
    if (s.empty()) return def;
    float v = wcstof(s.c_str(), nullptr);
    return (std::max)(lo, (std::min)(hi, v));
}

bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool def) {
    return ReadInt(path, section, key, def ? 1 : 0, 0, 1) != 0;
}

void WriteStr(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

void WriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int value) {
    WriteStr(path, section, key, std::to_wstring(value));
}

void WriteFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", value);
    WriteStr(path, section, key, buf);
}

std::wstring ColorToHex(COLORREF c) {
    wchar_t buf[8];
    swprintf_s(buf, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

COLORREF HexToColor(const std::wstring& hex, COLORREF def) {
    std::wstring h = hex;
    if (!h.empty() && h[0] == L'#') h.erase(0, 1);
    if (h.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(h.c_str(), &end, 16);
    if (!end || *end) return def;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// WritePrivateProfileString only writes Unicode if the file already starts with a UTF-16 BOM.
void EnsureUnicodeIni(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(file);
}

std::wstring g_exePath, g_exeDir, g_dataDir, g_logsDir, g_settingsFile;
bool g_portable = false;

bool IsDirectoryWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.write_test_" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

}  // namespace

bool AlertRule::operator==(const AlertRule& o) const {
    return name == o.name && enabled == o.enabled && processPattern == o.processPattern &&
           thresholdPercent == o.thresholdPercent && quietBeforeMs == o.quietBeforeMs && minLoudMs == o.minLoudMs &&
           cooldownMs == o.cooldownMs && suppressAfterInputMs == o.suppressAfterInputMs &&
           suppressScope == o.suppressScope && showBorder == o.showBorder && borderColor == o.borderColor &&
           borderThickness == o.borderThickness && flashDurationMs == o.flashDurationMs && blink == o.blink &&
           showLabel == o.showLabel && flashTaskbar == o.flashTaskbar;
}

std::wstring AlertRule::Describe() const {
    std::wostringstream o;
    o << L"'" << name << L"' " << (enabled ? L"enabled" : L"DISABLED") << L" pattern='" << processPattern
      << L"' threshold=" << thresholdPercent << L"% quietBefore=" << quietBeforeMs << L"ms minLoud=" << minLoudMs
      << L"ms cooldown=" << cooldownMs << L"ms suppressAfterInput=" << suppressAfterInputMs << L"ms ("
      << (suppressScope == 1 ? L"all windows" : L"focused window") << L") border=" << (showBorder ? L"on" : L"off")
      << L" #" << ColorToHex(borderColor) << L" " << borderThickness << L"px " << flashDurationMs << L"ms"
      << (blink ? L" blink" : L"") << (showLabel ? L" label" : L"") << L" taskbar=" << (flashTaskbar ? L"on" : L"off");
    return o.str();
}

AlertRule MakeWowFishingPreset() {
    AlertRule r;
    r.name = L"WoW fishing splash";
    // Retail, Classic, PTR and beta clients. Listed explicitly because "Wow*.exe"
    // would also match WowVoiceProxy.exe (the voice chat helper).
    r.processPattern = L"Wow.exe;WowT.exe;WowB.exe;WowClassic.exe;WowClassicT.exe;WowClassicB.exe";
    r.thresholdPercent = 20.0f;
    r.quietBeforeMs = 1500;
    r.minLoudMs = 0;
    // "Quiet before" already stops repeats. A long cooldown would let one false
    // alert (e.g. a loud cast) hide the real splash that follows it.
    r.cooldownMs = 1000;
    // Casting is triggered by input, so the cast sound right after a key press is ignored.
    r.suppressAfterInputMs = 1200;
    r.suppressScope = 0;
    r.borderColor = RGB(255, 40, 40);
    r.borderThickness = 14;
    r.flashDurationMs = 3000;
    return r;
}

bool Settings::Load(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    startTrackingOnLaunch = ReadBool(path, kGeneral, L"StartTrackingOnLaunch", startTrackingOnLaunch);
    debugPreset = ReadInt(path, kGeneral, L"DebugLevel", debugPreset, 0, 2);
    recordWindowTitles = ReadBool(path, kGeneral, L"RecordWindowTitles", recordWindowTitles);
    detection.soundThreshold = ReadFloat(path, kGeneral, L"SoundThreshold", detection.soundThreshold, 0.0001f, 0.5f);
    detection.soundReleaseMs = ReadInt(path, kGeneral, L"SoundReleaseMs", detection.soundReleaseMs, 50, 10000);
    detection.meterIntervalMs = ReadInt(path, kGeneral, L"MeterIntervalMs", detection.meterIntervalMs, 5, 250);

    hasWindowRect = ReadBool(path, kGeneral, L"HasWindowRect", false);
    windowRect.left = ReadInt(path, kGeneral, L"WindowLeft", 100, -32000, 32000);
    windowRect.top = ReadInt(path, kGeneral, L"WindowTop", 100, -32000, 32000);
    windowRect.right = ReadInt(path, kGeneral, L"WindowRight", 1300, -32000, 32000);
    windowRect.bottom = ReadInt(path, kGeneral, L"WindowBottom", 800, -32000, 32000);
    filterEnabled = ReadBool(path, kGeneral, L"FilterEnabled", false);
    filterText = ReadStr(path, kGeneral, L"FilterText", L"");

    alerts.clear();
    int count = ReadInt(path, L"Alerts", L"Count", 0, 0, 64);
    for (int i = 0; i < count; ++i) {
        std::wstring section = L"Alert." + std::to_wstring(i);
        const wchar_t* s = section.c_str();
        AlertRule r;
        r.name = ReadStr(path, s, L"Name", r.name);
        r.enabled = ReadBool(path, s, L"Enabled", r.enabled);
        r.processPattern = ReadStr(path, s, L"ProcessPattern", r.processPattern);
        r.thresholdPercent = ReadFloat(path, s, L"ThresholdPercent", r.thresholdPercent, 0.1f, 100.0f);
        r.quietBeforeMs = ReadInt(path, s, L"QuietBeforeMs", r.quietBeforeMs, 0, 60000);
        r.minLoudMs = ReadInt(path, s, L"MinLoudMs", r.minLoudMs, 0, 10000);
        r.cooldownMs = ReadInt(path, s, L"CooldownMs", r.cooldownMs, 0, 600000);
        r.suppressAfterInputMs = ReadInt(path, s, L"SuppressAfterInputMs", r.suppressAfterInputMs, 0, 60000);
        r.suppressScope = ReadInt(path, s, L"SuppressScope", r.suppressScope, 0, 1);
        r.showBorder = ReadBool(path, s, L"ShowBorder", r.showBorder);
        r.borderColor = HexToColor(ReadStr(path, s, L"BorderColor", ColorToHex(r.borderColor)), r.borderColor);
        r.borderThickness = ReadInt(path, s, L"BorderThickness", r.borderThickness, 2, 80);
        r.flashDurationMs = ReadInt(path, s, L"FlashDurationMs", r.flashDurationMs, 200, 30000);
        r.blink = ReadBool(path, s, L"Blink", r.blink);
        r.showLabel = ReadBool(path, s, L"ShowLabel", r.showLabel);
        r.flashTaskbar = ReadBool(path, s, L"FlashTaskbar", r.flashTaskbar);
        alerts.push_back(r);
    }
    return true;
}

bool Settings::Save(const std::wstring& path) const {
    EnsureUnicodeIni(path);
    WriteInt(path, kGeneral, L"StartTrackingOnLaunch", startTrackingOnLaunch ? 1 : 0);
    WriteInt(path, kGeneral, L"DebugLevel", debugPreset);
    WriteInt(path, kGeneral, L"RecordWindowTitles", recordWindowTitles ? 1 : 0);
    WriteFloat(path, kGeneral, L"SoundThreshold", detection.soundThreshold);
    WriteInt(path, kGeneral, L"SoundReleaseMs", detection.soundReleaseMs);
    WriteInt(path, kGeneral, L"MeterIntervalMs", detection.meterIntervalMs);
    WriteInt(path, kGeneral, L"HasWindowRect", hasWindowRect ? 1 : 0);
    WriteInt(path, kGeneral, L"WindowLeft", windowRect.left);
    WriteInt(path, kGeneral, L"WindowTop", windowRect.top);
    WriteInt(path, kGeneral, L"WindowRight", windowRect.right);
    WriteInt(path, kGeneral, L"WindowBottom", windowRect.bottom);
    WriteInt(path, kGeneral, L"FilterEnabled", filterEnabled ? 1 : 0);
    WriteStr(path, kGeneral, L"FilterText", filterText);

    // Drop sections of rules that were removed.
    int oldCount = ReadInt(path, L"Alerts", L"Count", 0, 0, 64);
    for (int i = static_cast<int>(alerts.size()); i < oldCount; ++i) {
        WritePrivateProfileStringW((L"Alert." + std::to_wstring(i)).c_str(), nullptr, nullptr, path.c_str());
    }
    WriteInt(path, L"Alerts", L"Count", static_cast<int>(alerts.size()));
    for (size_t i = 0; i < alerts.size(); ++i) {
        const AlertRule& r = alerts[i];
        std::wstring section = L"Alert." + std::to_wstring(i);
        const wchar_t* s = section.c_str();
        WriteStr(path, s, L"Name", r.name);
        WriteInt(path, s, L"Enabled", r.enabled ? 1 : 0);
        WriteStr(path, s, L"ProcessPattern", r.processPattern);
        WriteFloat(path, s, L"ThresholdPercent", r.thresholdPercent);
        WriteInt(path, s, L"QuietBeforeMs", r.quietBeforeMs);
        WriteInt(path, s, L"MinLoudMs", r.minLoudMs);
        WriteInt(path, s, L"CooldownMs", r.cooldownMs);
        WriteInt(path, s, L"SuppressAfterInputMs", r.suppressAfterInputMs);
        WriteInt(path, s, L"SuppressScope", r.suppressScope);
        WriteInt(path, s, L"ShowBorder", r.showBorder ? 1 : 0);
        WriteStr(path, s, L"BorderColor", ColorToHex(r.borderColor));
        WriteInt(path, s, L"BorderThickness", r.borderThickness);
        WriteInt(path, s, L"FlashDurationMs", r.flashDurationMs);
        WriteInt(path, s, L"Blink", r.blink ? 1 : 0);
        WriteInt(path, s, L"ShowLabel", r.showLabel ? 1 : 0);
        WriteInt(path, s, L"FlashTaskbar", r.flashTaskbar ? 1 : 0);
    }
    // Flush the profile cache so another process (or a crash) sees the file.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

std::wstring Settings::Describe() const {
    std::wostringstream o;
    o << L"startTrackingOnLaunch=" << startTrackingOnLaunch << L" debugLevel=" << debugPreset
      << L" recordWindowTitles=" << recordWindowTitles << L" soundThreshold=" << detection.soundThreshold
      << L" soundReleaseMs=" << detection.soundReleaseMs << L" meterIntervalMs=" << detection.meterIntervalMs
      << L" alertRules=" << alerts.size();
    for (size_t i = 0; i < alerts.size(); ++i) o << L"\n    rule[" << i << L"] " << alerts[i].Describe();
    return o.str();
}

namespace AppPaths {

void Init(const std::wstring& overrideDataDir) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    buf.resize(n);
    g_exePath = buf;
    size_t slash = buf.find_last_of(L"\\/");
    g_exeDir = slash == std::wstring::npos ? L"." : buf.substr(0, slash);

    if (!overrideDataDir.empty()) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(overrideDataDir.c_str(), MAX_PATH, full, nullptr);
        g_dataDir = full;
        SHCreateDirectoryExW(nullptr, g_dataDir.c_str(), nullptr);
        g_portable = true;
    } else if (IsDirectoryWritable(g_exeDir)) {
        g_dataDir = g_exeDir;
        g_portable = true;
    } else {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
            g_dataDir = std::wstring(local) + L"\\WindowsSoundTracker";
            CoTaskMemFree(local);
        } else {
            g_dataDir = g_exeDir;
        }
        SHCreateDirectoryExW(nullptr, g_dataDir.c_str(), nullptr);
        g_portable = false;
    }
    g_logsDir = g_dataDir + L"\\logs";
    CreateDirectoryW(g_logsDir.c_str(), nullptr);
    g_settingsFile = g_dataDir + L"\\SoundTracker.ini";
}

const std::wstring& ExeDir() { return g_exeDir; }
const std::wstring& ExePath() { return g_exePath; }
const std::wstring& DataDir() { return g_dataDir; }
const std::wstring& LogsDir() { return g_logsDir; }
const std::wstring& SettingsFile() { return g_settingsFile; }
bool IsPortable() { return g_portable; }

}  // namespace AppPaths
