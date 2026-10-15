#include "ProcessInfo.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <cwctype>

namespace procinfo {

namespace {

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool CreationTime(DWORD pid, ULONGLONG& out) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created, exited, kernel, user;
    bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (ok) out = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return ok;
}

}  // namespace

std::wstring QueryImagePath(DWORD pid, DWORD* error) {
    if (error) *error = 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        if (error) *error = GetLastError();
        return std::wstring();
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, &path[0], &size)) {
        path.resize(size);
    } else {
        if (error) *error = GetLastError();
        path.clear();
    }
    CloseHandle(process);
    return path;
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

DWORD ParentPid(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!parent) return 0;
    // A parent that started after the child is a different process that reused the PID.
    ULONGLONG childCreated = 0, parentCreated = 0;
    if (CreationTime(pid, childCreated) && CreationTime(parent, parentCreated) && parentCreated > childCreated) {
        return 0;
    }
    return parent;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::vector<HWND> FindMainWindows(DWORD pid) {
    struct Data {
        DWORD pid;
        std::vector<HWND> windows;
    } data{pid, {}};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* d = reinterpret_cast<Data*>(lParam);
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid != d->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            if (IsCloaked(hwnd)) return TRUE;
            RECT rc;
            if (!IsIconic(hwnd) && GetWindowRect(hwnd, &rc) && (rc.right - rc.left < 50 || rc.bottom - rc.top < 50)) {
                return TRUE;
            }
            d->windows.push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));
    return data.windows;
}

WindowOwner ResolveWindowOwner(DWORD pid, const std::wstring& imageName) {
    WindowOwner owner;
    owner.pid = pid;
    if (pid == 0) return owner;
    owner.windows = FindMainWindows(pid);
    if (!owner.windows.empty()) return owner;

    // WebView2 audio belongs to whichever app hosts it, whatever its name.
    bool anyParent = EqualsNoCase(imageName, L"msedgewebview2.exe");
    DWORD current = pid;
    for (int depth = 1; depth <= 4; ++depth) {
        DWORD parent = ParentPid(current);
        if (!parent || parent == current) break;
        std::wstring parentName = FileName(QueryImagePath(parent));
        if (!anyParent && !EqualsNoCase(parentName, imageName)) break;
        auto windows = FindMainWindows(parent);
        if (!windows.empty()) {
            owner.pid = parent;
            owner.windows = std::move(windows);
            owner.depth = depth;
            return owner;
        }
        current = parent;
        anyParent = false;
    }
    return owner;
}

std::wstring WindowTitle(HWND hwnd) {
    // InternalGetWindowText reads the caption directly. GetWindowText would send
    // WM_GETTEXT for windows of our own process, blocking the meter thread
    // whenever the UI thread is busy (seen as a 614 ms stall in a debug log).
    wchar_t title[512] = {};
    int len = InternalGetWindowText(hwnd, title, 512);
    return std::wstring(title, len > 0 ? len : 0);
}

std::wstring DescribeWindows(const std::vector<HWND>& windows) {
    if (windows.empty()) return std::wstring();
    std::wstring title = WindowTitle(windows.front());
    if (title.empty()) title = L"(untitled window)";
    if (title.size() > 120) title = title.substr(0, 117) + L"...";
    if (windows.size() > 1) title += L" (+" + std::to_wstring(windows.size() - 1) + L" more windows)";
    return title;
}

}  // namespace procinfo
