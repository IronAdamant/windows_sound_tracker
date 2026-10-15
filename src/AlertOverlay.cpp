#include "AlertOverlay.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include "DebugLog.h"
#include "ProcessInfo.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

const wchar_t* kOverlayClass = L"SoundTrackerAlertOverlay";
const COLORREF kKeyColor = RGB(1, 0, 1);  // Transparent pixels
const UINT_PTR kTimerId = 1;
const UINT kTickMs = 40;
const UINT kBlinkMs = 300;

// The main window is DPI-unaware (it relies on Windows scaling), but borders must
// line up with other windows exactly, so overlay work runs per-monitor aware.
class PerMonitorDpiScope {
public:
    PerMonitorDpiScope() {
        using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        static Fn fn = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
        m_fn = fn;
        if (m_fn) m_previous = m_fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~PerMonitorDpiScope() {
        if (m_fn && m_previous) m_fn(m_previous);
    }
    PerMonitorDpiScope(const PerMonitorDpiScope&) = delete;
    PerMonitorDpiScope& operator=(const PerMonitorDpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT(WINAPI* m_fn)(DPI_AWARENESS_CONTEXT) = nullptr;
    DPI_AWARENESS_CONTEXT m_previous = nullptr;
};

bool TargetBounds(HWND target, RECT& rc) {
    // Visible frame without the invisible resize borders Windows 10/11 adds.
    if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))) &&
        rc.right > rc.left && rc.bottom > rc.top) {
        return true;
    }
    return GetWindowRect(target, &rc) && rc.right > rc.left && rc.bottom > rc.top;
}

// Scale of the monitor the target is on. GetDpiForWindow would report 96 for
// DPI-unaware targets even on a 150% monitor.
UINT TargetDpi(HWND target) {
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) return 96;
    return dpiX;
}

}  // namespace

struct AlertOverlayManager::Overlay {
    AlertOverlayManager* manager = nullptr;
    HWND target = nullptr;
    AlertRule style;
    std::wstring label;
    ULONGLONG startTick = 0;
    RECT lastRect{};
    bool visible = false;
    HFONT font = nullptr;
    UINT fontDpi = 0;
};

AlertOverlayManager::AlertOverlayManager(HINSTANCE instance) : m_instance(instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    m_classRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    if (!m_classRegistered) LOG_ERROR("overlay", L"RegisterClassEx failed: " << dbg::Win32ErrorToString(GetLastError()));
}

AlertOverlayManager::~AlertOverlayManager() { CloseAll(); }

void AlertOverlayManager::CloseAll() {
    auto copy = m_overlayByTarget;
    for (auto& entry : copy) DestroyWindow(entry.second);
    m_overlayByTarget.clear();
}

void AlertOverlayManager::Flash(const std::vector<HWND>& targets, const AlertRule& style, const std::wstring& label) {
    for (HWND target : targets) {
        if (!IsWindow(target)) {
            LOG_WARN("overlay", L"Target window " << static_cast<void*>(target) << L" no longer exists");
            continue;
        }
        if (style.showBorder) ShowBorder(target, style, label);
        if (style.flashTaskbar) {
            FLASHWINFO fi{};
            fi.cbSize = sizeof(fi);
            fi.hwnd = target;
            fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;  // Until the user switches to that window
            FlashWindowEx(&fi);
            LOG_DEBUG("overlay", L"Taskbar flash requested for " << static_cast<void*>(target));
        }
    }
}

void AlertOverlayManager::ShowBorder(HWND target, const AlertRule& style, const std::wstring& label) {
    PerMonitorDpiScope dpiScope;
    RECT rc{};
    bool hasBounds = TargetBounds(target, rc);
    std::wstring title = procinfo::WindowTitle(target);
    if (IsIconic(target) || procinfo::IsCloaked(target) || !hasBounds) {
        LOG_INFO("overlay", L"Border skipped for '" << title << L"' (" << static_cast<void*>(target) << L"): "
                                << (IsIconic(target) ? L"minimized" : !hasBounds ? L"no bounds" : L"on another virtual desktop")
                                << (style.flashTaskbar ? L"; taskbar flash only" : L""));
        return;
    }

    auto existing = m_overlayByTarget.find(target);
    if (existing != m_overlayByTarget.end() && IsWindow(existing->second)) {
        auto* overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(existing->second, GWLP_USERDATA));
        if (overlay) {
            overlay->style = style;
            overlay->label = label;
            overlay->startTick = GetTickCount64();
            InvalidateRect(existing->second, nullptr, TRUE);
            LOG_INFO("overlay", L"Border restarted on '" << title << L"'");
            return;
        }
    }

    auto* overlay = new Overlay();
    overlay->manager = this;
    overlay->target = target;
    overlay->style = style;
    overlay->label = label;
    overlay->startTick = GetTickCount64();
    overlay->lastRect = rc;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kOverlayClass, L"Sound Tracker alert", WS_POPUP, rc.left, rc.top, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, m_instance, overlay);
    if (!hwnd) {
        LOG_ERROR("overlay", L"CreateWindowEx for border failed: " << dbg::Win32ErrorToString(GetLastError()));
        delete overlay;
        return;
    }
    SetLayeredWindowAttributes(hwnd, kKeyColor, 235, LWA_COLORKEY | LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    overlay->visible = true;
    SetTimer(hwnd, kTimerId, kTickMs, nullptr);
    m_overlayByTarget[target] = hwnd;
    LOG_INFO("overlay", L"Border shown on '" << title << L"' (" << static_cast<void*>(target) << L") at " << rc.left << L","
                            << rc.top << L" " << (rc.right - rc.left) << L"x" << (rc.bottom - rc.top) << L" dpi "
                            << TargetDpi(target) << L", label '" << label << L"'");
}

LRESULT CALLBACK AlertOverlayManager::OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER: {
            if (!overlay || wParam != kTimerId) break;
            ULONGLONG elapsed = GetTickCount64() - overlay->startTick;
            HWND target = overlay->target;
            if (elapsed >= static_cast<ULONGLONG>(overlay->style.flashDurationMs) || !IsWindow(target)) {
                DestroyWindow(hwnd);
                return 0;
            }
            PerMonitorDpiScope dpiScope;
            RECT rc{};
            bool show = !IsIconic(target) && !procinfo::IsCloaked(target) && TargetBounds(target, rc);
            if (show && overlay->style.blink) show = (elapsed / kBlinkMs) % 2 == 0;
            if (show) {
                bool moved = !EqualRect(&rc, &overlay->lastRect);
                // Re-assert topmost every tick in case the target was brought to the front.
                SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (moved) {
                    overlay->lastRect = rc;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                overlay->visible = true;
            } else if (overlay->visible) {
                ShowWindow(hwnd, SW_HIDE);
                overlay->visible = false;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (overlay) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH key = CreateSolidBrush(kKeyColor);
                FillRect(dc, &rc, key);
                DeleteObject(key);

                UINT dpi = TargetDpi(overlay->target);
                int t = MulDiv(overlay->style.borderThickness, static_cast<int>(dpi), 96);
                HBRUSH brush = CreateSolidBrush(overlay->style.borderColor);
                RECT edges[4] = {{0, 0, rc.right, t}, {0, rc.bottom - t, rc.right, rc.bottom}, {0, 0, t, rc.bottom},
                                 {rc.right - t, 0, rc.right, rc.bottom}};
                for (auto& edge : edges) FillRect(dc, &edge, brush);

                if (overlay->style.showLabel && !overlay->label.empty()) {
                    if (!overlay->font || overlay->fontDpi != dpi) {
                        if (overlay->font) DeleteObject(overlay->font);
                        overlay->font = CreateFontW(-MulDiv(15, static_cast<int>(dpi), 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                        overlay->fontDpi = dpi;
                    }
                    HGDIOBJ oldFont = SelectObject(dc, overlay->font);
                    RECT textRc{0, 0, 0, 0};
                    DrawTextW(dc, overlay->label.c_str(), -1, &textRc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    int pad = MulDiv(6, static_cast<int>(dpi), 96);
                    RECT box{t, t, t + (textRc.right - textRc.left) + pad * 2, t + (textRc.bottom - textRc.top) + pad};
                    if (box.right > rc.right - t) box.right = rc.right - t;
                    FillRect(dc, &box, brush);
                    COLORREF c = overlay->style.borderColor;
                    int luminance = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
                    SetTextColor(dc, luminance > 150 ? RGB(0, 0, 0) : RGB(255, 255, 255));
                    SetBkMode(dc, TRANSPARENT);
                    RECT inner{box.left + pad, box.top + pad / 2, box.right - pad, box.bottom};
                    DrawTextW(dc, overlay->label.c_str(), -1, &inner, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                    SelectObject(dc, oldFont);
                }
                DeleteObject(brush);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            if (overlay) {
                KillTimer(hwnd, kTimerId);
                auto& map = overlay->manager->m_overlayByTarget;
                auto it = map.find(overlay->target);
                if (it != map.end() && it->second == hwnd) map.erase(it);
                if (overlay->font) DeleteObject(overlay->font);
                delete overlay;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
