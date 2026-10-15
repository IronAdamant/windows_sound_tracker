#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Process and window lookups. Everything here is safe to call from any thread
// and needs no elevation (PROCESS_QUERY_LIMITED_INFORMATION only).
namespace procinfo {

// Full image path, or empty on failure (error code in *error if given).
std::wstring QueryImagePath(DWORD pid, DWORD* error = nullptr);
std::wstring FileName(const std::wstring& path);

// Parent process id, or 0 if unknown or the parent has exited (PID reuse check).
DWORD ParentPid(DWORD pid);

// Visible, unowned, non-tool, non-cloaked top-level windows of a process, in z-order.
std::vector<HWND> FindMainWindows(DWORD pid);

// The process whose windows represent this audio session. Browsers and Electron
// apps play audio from a child process that has no window, so walk up to the
// parent while it is the same executable.
struct WindowOwner {
    DWORD pid = 0;
    std::vector<HWND> windows;
    int depth = 0;  // 0 = the session's own process
};
WindowOwner ResolveWindowOwner(DWORD pid, const std::wstring& imageName);

std::wstring WindowTitle(HWND hwnd);
// "Title" or "Title (+2 more windows)".
std::wstring DescribeWindows(const std::vector<HWND>& windows);
bool IsCloaked(HWND hwnd);

}  // namespace procinfo
