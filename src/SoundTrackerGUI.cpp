#include "SoundTrackerGUI.h"

#include <windowsx.h>
#include <algorithm>
#include <cwctype>

#include "../resource.h"
#include "DebugLog.h"
#include "ProcessInfo.h"
#include "Version.h"

namespace {

const wchar_t* kWindowClass = L"SoundTrackerWindow";
const wchar_t* kTestingGuideUrl = L"https://github.com/IronAdamant/windows_sound_tracker/blob/master/docs/TESTING.md";

enum Column { ColTime, ColDuration, ColProcess, ColPid, ColWindow, ColDescription, ColPeak, ColVolume, ColAlerts, ColDevice, ColPath, ColCount };

// The active level can come from --debug/--verbose without being saved.
int CurrentPreset() {
    switch (dbg::GetLevel()) {
        case dbg::Level::Trace: return 2;
        case dbg::Level::Debug: return 1;
        default: return 0;
    }
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

}  // namespace

SoundTrackerGUI::SoundTrackerGUI(Settings& settings) : m_settings(settings) {}

SoundTrackerGUI::~SoundTrackerGUI() {
    if (m_tracker) m_tracker->Stop();
    m_alertsDialog.reset();
    m_overlays.reset();
    if (m_hFont) DeleteObject(m_hFont);
    if (m_hBoldFont) DeleteObject(m_hBoldFont);
}

bool SoundTrackerGUI::Initialize(HINSTANCE hInstance, const LaunchOptions& options) {
    m_instance = hInstance;
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    m_tracker = std::make_unique<SoundTracker>(AppPaths::LogsDir());
    m_tracker->SetDetection(m_settings.detection);
    m_tracker->SetRules(m_settings.alerts);
    m_tracker->SetRecordWindowTitles(m_settings.recordWindowTitles);

    if (!CreateMainWindow(hInstance)) {
        LOG_ERROR("ui", L"Cannot create main window: " << dbg::Win32ErrorToString(GetLastError()));
        return false;
    }

    HWND target = m_hWnd;
    m_tracker->SetAlertCallback([target](const AlertFire& fire) {
        auto* copy = new AlertFire(fire);
        if (!PostMessageW(target, WM_APP_ALERT, 0, reinterpret_cast<LPARAM>(copy))) {
            LOG_WARN("ui", L"Could not post alert to the main window: " << dbg::Win32ErrorToString(GetLastError()));
            delete copy;
        }
    });
    m_overlays = std::make_unique<AlertOverlayManager>(hInstance);
    m_alertsDialog = std::make_unique<AlertsDialog>(hInstance, m_hWnd, *this);

    SetTimer(m_hWnd, ID_TIMER_UPDATE, 250, nullptr);
    if (options.exitAfterSeconds > 0) {
        LOG_INFO("ui", L"Will exit automatically after " << options.exitAfterSeconds << L" s (--exit-after)");
        SetTimer(m_hWnd, ID_TIMER_EXIT, static_cast<UINT>(options.exitAfterSeconds) * 1000, nullptr);
    }
    if (options.startTracking || m_settings.startTrackingOnLaunch) OnStartStop();
    UpdateStatusBar();
    return true;
}

bool SoundTrackerGUI::CreateMainWindow(HINSTANCE hInstance) {
    m_hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    m_hIconSmall = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!m_hIcon) m_hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!m_hIconSmall) m_hIconSmall = m_hIcon;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = m_hIcon;
    wc.hIconSm = m_hIconSmall;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return false;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1300, h = 720;
    if (m_settings.hasWindowRect && MonitorFromRect(&m_settings.windowRect, MONITOR_DEFAULTTONULL)) {
        x = m_settings.windowRect.left;
        y = m_settings.windowRect.top;
        w = (std::max)(600L, m_settings.windowRect.right - m_settings.windowRect.left);
        h = (std::max)(400L, m_settings.windowRect.bottom - m_settings.windowRect.top);
    }

    std::wstring title = std::wstring(APP_NAME_WSTR) + L" " + APP_VERSION_WSTR;
    m_hWnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                             hInstance, this);
    if (!m_hWnd) return false;

    m_hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    m_hBoldFont = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    CreateMenuBar();
    CreateControls();
    LayoutControls();
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);
    return true;
}

void SoundTrackerGUI::CreateMenuBar() {
    m_hMenu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_MENU_START_STOP, L"&Start / Stop Tracking");
    AppendMenuW(file, MF_STRING, ID_MENU_CLEAR, L"&Clear List");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_MENU_OPEN_LOGS, L"Open &Logs Folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_MENU_EXIT, L"E&xit");
    AppendMenuW(m_hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU options = CreatePopupMenu();
    AppendMenuW(options, MF_STRING, ID_MENU_ALERTS, L"Window &Alerts...");
    AppendMenuW(options, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(options, MF_STRING, ID_MENU_AUTOSTART, L"Start Tracking on &Launch");
    AppendMenuW(options, MF_STRING, ID_MENU_WINDOW_TITLES, L"Record &Window Titles");
    HMENU debug = CreatePopupMenu();
    AppendMenuW(debug, MF_STRING, ID_MENU_DEBUG_NORMAL, L"&Normal (errors, warnings, key events)");
    AppendMenuW(debug, MF_STRING, ID_MENU_DEBUG_DEBUG, L"&Debug (every sound and alert decision)");
    AppendMenuW(debug, MF_STRING, ID_MENU_DEBUG_VERBOSE, L"&Verbose (everything, incl. level envelopes)");
    AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(debug), L"&Debug Logging");
    AppendMenuW(m_hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(options), L"&Options");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_MENU_OPEN_DEBUG_LOG, L"Show Current &Debug Log");
    AppendMenuW(help, MF_STRING, ID_MENU_TESTING_GUIDE, L"&Testing Guide (web)");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, ID_MENU_ABOUT, L"&About");
    AppendMenuW(m_hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(m_hWnd, m_hMenu);
    UpdateMenuChecks();
}

void SoundTrackerGUI::UpdateMenuChecks() {
    if (!m_hMenu) return;
    CheckMenuItem(m_hMenu, ID_MENU_AUTOSTART, m_settings.startTrackingOnLaunch ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_hMenu, ID_MENU_WINDOW_TITLES, m_settings.recordWindowTitles ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem(m_hMenu, ID_MENU_DEBUG_NORMAL, ID_MENU_DEBUG_VERBOSE, ID_MENU_DEBUG_NORMAL + CurrentPreset(), MF_BYCOMMAND);
}

void SoundTrackerGUI::CreateControls() {
    auto button = [&](int id, const wchar_t* text, int x, int w, HFONT font) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, 10, w, 35, m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    };
    m_hButtonStartStop = button(ID_BUTTON_START_STOP, L"Start Tracking", 10, 150, m_hBoldFont);
    m_hButtonAlerts = button(ID_BUTTON_ALERTS, L"Window Alerts...", 170, 140, m_hFont);
    m_hButtonClear = button(ID_BUTTON_CLEAR, L"Clear", 320, 80, m_hFont);

    m_hFilterCheckbox = CreateWindowExW(0, L"BUTTON", L"Filter:", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 415, 15,
                                        70, 25, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHECKBOX_FILTER)), m_instance, nullptr);
    SendMessageW(m_hFilterCheckbox, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    m_hFilterEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_settings.filterText.c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 490, 13, 240, 28, m_hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_FILTER)), m_instance, nullptr);
    SendMessageW(m_hFilterEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    SendMessageW(m_hFilterEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"process, window or description"));
    Button_SetCheck(m_hFilterCheckbox, m_settings.filterEnabled ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(m_hFilterEdit, m_settings.filterEnabled);

    CreateListView();
    CreateStatusBar();
    CreateTrayIcon();
}

void SoundTrackerGUI::CreateListView() {
    m_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
                                  10, 55, 100, 100, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LISTVIEW)), m_instance, nullptr);
    ListView_SetExtendedListViewStyle(m_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    struct Col { const wchar_t* name; int width; };
    const Col cols[ColCount] = {{L"Time", 75}, {L"Duration", 75}, {L"Process", 140}, {L"PID", 60}, {L"Window", 220},
                                {L"Description", 220}, {L"Peak", 60}, {L"Volume", 60}, {L"Alerts", 50}, {L"Device", 170}, {L"Path", 320}};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<LPWSTR>(cols[i].name);
        c.cx = cols[i].width;
        c.iSubItem = i;
        ListView_InsertColumn(m_hListView, i, &c);
    }
    SendMessageW(m_hListView, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
}

void SoundTrackerGUI::CreateStatusBar() {
    m_hStatusBar = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUSBAR)), m_instance, nullptr);
    int parts[] = {250, 380, 520, 640, -1};
    SendMessageW(m_hStatusBar, SB_SETPARTS, 5, reinterpret_cast<LPARAM>(parts));
    SetWindowLongPtrW(m_hStatusBar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_originalStatusProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(m_hStatusBar, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(StatusBarProc)));
}

void SoundTrackerGUI::CreateTrayIcon() {
    m_trayIcon.cbSize = sizeof(m_trayIcon);
    m_trayIcon.hWnd = m_hWnd;
    m_trayIcon.uID = ID_TRAY_ICON;
    m_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_trayIcon.uCallbackMessage = WM_TRAYICON;
    m_trayIcon.hIcon = m_hIconSmall;
    wcscpy_s(m_trayIcon.szTip, L"Sound Tracker - double-click to restore");
}

void SoundTrackerGUI::LayoutControls() {
    if (!m_hListView || !m_hStatusBar) return;  // WM_SIZE arrives during window creation
    RECT rc;
    GetClientRect(m_hWnd, &rc);
    SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
    RECT status{};
    GetWindowRect(m_hStatusBar, &status);
    int statusHeight = status.bottom - status.top;
    SetWindowPos(m_hListView, nullptr, 10, 55, (std::max)(0L, rc.right - 20), (std::max)(0L, rc.bottom - 55 - statusHeight - 8), SWP_NOZORDER);
}

LRESULT CALLBACK SoundTrackerGUI::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SoundTrackerGUI* self = nullptr;
    if (uMsg == WM_NCCREATE) {
        self = static_cast<SoundTrackerGUI*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hWnd = hWnd;
    } else {
        self = reinterpret_cast<SoundTrackerGUI*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }
    if (self && self->m_hWnd) return self->HandleMessage(uMsg, wParam, lParam);
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK SoundTrackerGUI::StatusBarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SoundTrackerGUI*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self && uMsg == WM_LBUTTONUP) {
        int parts[5] = {};
        SendMessageW(hWnd, SB_GETPARTS, 5, reinterpret_cast<LPARAM>(parts));
        if (GET_X_LPARAM(lParam) > parts[3]) {
            self->OnStatusBarClick();
            return 0;
        }
    }
    if (self && self->m_originalStatusProc) return CallWindowProcW(self->m_originalStatusProc, hWnd, uMsg, wParam, lParam);
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT SoundTrackerGUI::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BUTTON_START_STOP:
                case ID_MENU_START_STOP: OnStartStop(); break;
                case ID_BUTTON_CLEAR:
                case ID_MENU_CLEAR: OnClear(); break;
                case ID_BUTTON_ALERTS:
                case ID_MENU_ALERTS:
                    if (m_inTray) RestoreFromTray();
                    m_alertsDialog->Show();
                    break;
                case ID_CHECKBOX_FILTER: OnFilterChanged(); break;
                case ID_EDIT_FILTER:
                    if (HIWORD(wParam) == EN_CHANGE && m_settings.filterEnabled) UpdateListView(true);
                    break;
                case ID_MENU_RESTORE: RestoreFromTray(); break;
                case ID_MENU_EXIT: PostMessageW(m_hWnd, WM_CLOSE, 0, 0); break;
                case ID_MENU_OPEN_LOGS: OpenLogsFolder(m_tracker->GetCurrentLogPath()); break;
                case ID_MENU_OPEN_DEBUG_LOG: OpenLogsFolder(dbg::CurrentLogPath()); break;
                case ID_MENU_AUTOSTART:
                    m_settings.startTrackingOnLaunch = !m_settings.startTrackingOnLaunch;
                    SaveSettings();
                    UpdateMenuChecks();
                    break;
                case ID_MENU_WINDOW_TITLES:
                    m_settings.recordWindowTitles = !m_settings.recordWindowTitles;
                    m_tracker->SetRecordWindowTitles(m_settings.recordWindowTitles);
                    SaveSettings();
                    UpdateMenuChecks();
                    break;
                case ID_MENU_DEBUG_NORMAL: SetDebugPreset(0); break;
                case ID_MENU_DEBUG_DEBUG: SetDebugPreset(1); break;
                case ID_MENU_DEBUG_VERBOSE: SetDebugPreset(2); break;
                case ID_MENU_TESTING_GUIDE: ShellExecuteW(m_hWnd, L"open", kTestingGuideUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
                case ID_MENU_ABOUT: ShowAbout(); break;
            }
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (hdr->idFrom == ID_LISTVIEW) {
                if (hdr->code == LVN_GETDISPINFOW) OnListDispInfo(reinterpret_cast<NMLVDISPINFOW*>(lParam));
                else if (hdr->code == NM_CLICK) OnListViewClick();
            }
            return 0;
        }

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                MinimizeToTray();
            } else {
                LayoutControls();
            }
            return 0;

        case WM_TRAYICON:
            OnTrayIcon(lParam);
            return 0;

        case WM_APP_ALERT:
            OnAlert(reinterpret_cast<AlertFire*>(lParam));
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_UPDATE) {
                UpdateListView(false);
                UpdateStatusBar();
            } else if (wParam == ID_TIMER_EXIT) {
                KillTimer(m_hWnd, ID_TIMER_EXIT);
                LOG_INFO("ui", L"Exit timer elapsed, closing");
                m_exitTimerFired = true;
                PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
            }
            return 0;

        case WM_CLOSE:
            if (m_isTracking && !m_exitTimerFired) {
                if (m_inTray) RestoreFromTray();
                if (MessageBoxW(m_hWnd, L"Sound tracking is active. Stop tracking and exit?", L"Confirm Exit",
                                MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
            }
            SaveSettings();
            if (m_isTracking) {
                m_tracker->Stop();
                m_isTracking = false;
            }
            m_overlays->CloseAll();
            DestroyWindow(m_hWnd);
            return 0;

        case WM_DESTROY:
            KillTimer(m_hWnd, ID_TIMER_UPDATE);
            if (m_inTray) Shell_NotifyIconW(NIM_DELETE, &m_trayIcon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(m_hWnd, uMsg, wParam, lParam);
}

void SoundTrackerGUI::OnStartStop() {
    if (m_isTracking) {
        m_tracker->Stop();
        m_isTracking = false;
        SetWindowTextW(m_hButtonStartStop, L"Start Tracking");
    } else {
        m_tracker->Start();
        m_isTracking = true;
        SetWindowTextW(m_hButtonStartStop, L"Stop Tracking");
    }
    m_lastVersion = ~0ull;
    UpdateListView(true);
    UpdateStatusBar();
}

void SoundTrackerGUI::OnClear() {
    m_clearedAfterId = m_tracker->GetLastEventId();
    UpdateListView(true);
}

void SoundTrackerGUI::OnFilterChanged() {
    m_settings.filterEnabled = Button_GetCheck(m_hFilterCheckbox) == BST_CHECKED;
    EnableWindow(m_hFilterEdit, m_settings.filterEnabled);
    if (m_settings.filterEnabled) SetFocus(m_hFilterEdit);
    UpdateListView(true);
}

void SoundTrackerGUI::UpdateListView(bool force) {
    uint64_t version = m_tracker->GetEventsVersion();
    if (!force && version == m_lastVersion) return;
    m_lastVersion = version;

    uint64_t selectedId = 0;
    int selected = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (selected >= 0 && selected < static_cast<int>(m_rows.size())) selectedId = m_rows[static_cast<size_t>(selected)].id;

    std::wstring filter;
    if (m_settings.filterEnabled) {
        int len = GetWindowTextLengthW(m_hFilterEdit);
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(m_hFilterEdit, &text[0], len + 1);
        text.resize(static_cast<size_t>(len));
        m_settings.filterText = text;
        filter = Lower(text);
    }

    auto events = m_tracker->GetEventsSnapshot(m_clearedAfterId, 2000);
    m_rows.clear();
    m_rows.reserve(events.size());
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (!filter.empty() && Lower(it->processName).find(filter) == std::wstring::npos &&
            Lower(it->soundDescription).find(filter) == std::wstring::npos &&
            Lower(it->windowTitle).find(filter) == std::wstring::npos) {
            continue;
        }
        m_rows.push_back(std::move(*it));
    }
    ListView_SetItemCountEx(m_hListView, static_cast<int>(m_rows.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    if (selectedId) {
        ListView_SetItemState(m_hListView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].id == selectedId) {
                ListView_SetItemState(m_hListView, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
    InvalidateRect(m_hListView, nullptr, FALSE);
}

std::wstring SoundTrackerGUI::CellText(const AudioEvent& e, int column) const {
    wchar_t buf[64];
    switch (column) {
        case ColTime: {
            auto t = std::chrono::system_clock::to_time_t(e.timestamp);
            std::tm tm{};
            localtime_s(&tm, &t);
            swprintf_s(buf, L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            return buf;
        }
        case ColDuration:
            if (e.durationMs < 1000) swprintf_s(buf, L"%lu ms", e.durationMs);
            else swprintf_s(buf, L"%.1f s", e.durationMs / 1000.0);
            return e.ongoing ? std::wstring(buf) + L" \x25B6" : std::wstring(buf);
        case ColProcess: return e.processName;
        case ColPid: return std::to_wstring(e.processId);
        case ColWindow: return e.windowTitle;
        case ColDescription: return e.soundDescription;
        case ColPeak: swprintf_s(buf, L"%.0f%%", e.peakLevel * 100); return buf;
        case ColVolume: swprintf_s(buf, L"%.0f%%", e.volumeLevel * 100); return buf;
        case ColAlerts: return e.alertsFired ? std::to_wstring(e.alertsFired) : std::wstring();
        case ColDevice: return e.deviceName;
        case ColPath: return e.processPath;
    }
    return std::wstring();
}

void SoundTrackerGUI::OnListDispInfo(NMLVDISPINFOW* info) {
    if (!(info->item.mask & LVIF_TEXT)) return;
    int row = info->item.iItem;
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    std::wstring text = CellText(m_rows[static_cast<size_t>(row)], info->item.iSubItem);
    wcsncpy_s(info->item.pszText, static_cast<size_t>(info->item.cchTextMax), text.c_str(), _TRUNCATE);
}

void SoundTrackerGUI::UpdateStatusBar() {
    std::wstring status = std::wstring(m_isTracking ? L"Recording" : L"Stopped") + L" | Log level: " +
                          dbg::PresetName(static_cast<dbg::Preset>(CurrentPreset()));
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(status.c_str()));

    std::wstring sounds = L"Sounds: " + std::to_wstring(m_tracker->GetSoundCount());
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(sounds.c_str()));

    long long secs = m_isTracking ? std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now() - m_tracker->GetStartTime()).count()
                                  : 0;
    wchar_t duration[64];
    swprintf_s(duration, L"Duration: %02lld:%02lld:%02lld", secs / 3600, (secs % 3600) / 60, secs % 60);
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(duration));

    size_t enabledRules = std::count_if(m_settings.alerts.begin(), m_settings.alerts.end(), [](const AlertRule& r) { return r.enabled; });
    std::wstring alerts = L"Alerts: " + std::to_wstring(m_tracker->GetAlertCount()) + L" (" + std::to_wstring(enabledRules) + (enabledRules == 1 ? L" rule)" : L" rules)");
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(alerts.c_str()));

    std::wstring logPath = m_tracker->GetCurrentLogPath();
    std::wstring logText = logPath.empty() ? L"Logs: " + AppPaths::LogsDir() + L" (click to open)" : L"Log: " + logPath + L" (click to open)";
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 4, reinterpret_cast<LPARAM>(logText.c_str()));
}

void SoundTrackerGUI::OnListViewClick() {
    int selected = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(m_rows.size())) return;
    std::wstring row;
    for (int col = 0; col < ColCount; ++col) {
        if (col) row += L"\t";
        row += CellText(m_rows[static_cast<size_t>(selected)], col);
    }
    if (!OpenClipboard(m_hWnd)) return;
    EmptyClipboard();
    size_t bytes = (row.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        if (auto* dst = static_cast<wchar_t*>(GlobalLock(mem))) {
            memcpy(dst, row.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 4, reinterpret_cast<LPARAM>(L"Row copied to clipboard"));
}

void SoundTrackerGUI::OnStatusBarClick() { OpenLogsFolder(m_tracker->GetCurrentLogPath()); }

void SoundTrackerGUI::OpenLogsFolder(const std::wstring& selectFile) {
    if (!selectFile.empty() && GetFileAttributesW(selectFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring args = L"/select,\"" + selectFile + L"\"";
        ShellExecuteW(m_hWnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(m_hWnd, L"open", AppPaths::LogsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void SoundTrackerGUI::OnAlert(AlertFire* fire) {
    std::unique_ptr<AlertFire> owned(fire);
    if (!fire || !m_overlays) return;
    auto dispatchMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - fire->detectedAt).count();
    std::vector<HWND> windows = procinfo::FindMainWindows(fire->windowOwnerPid);
    if (windows.empty()) windows = procinfo::ResolveWindowOwner(fire->processId, fire->processName).windows;
    if (windows.empty()) {
        LOG_WARN("alerts", L"Alert '" << fire->rule.name << L"' fired for " << fire->processName << L" PID " << fire->processId
                                      << L" but it has no visible window to highlight");
        return;
    }
    std::wstring label = fire->rule.name + L": " + fire->processName + L" (PID " + std::to_wstring(fire->processId) + L")";
    if (windows.size() > 1) label += L" - " + std::to_wstring(windows.size()) + L" windows, can't tell which";
    LOG_DEBUG("alerts", L"Dispatching alert to " << windows.size() << L" window(s), " << dispatchMs << L" ms after detection");
    m_overlays->Flash(windows, fire->rule, label);
}

void SoundTrackerGUI::TestFlash(DWORD pid, const std::wstring& processName, const AlertRule& style, const std::wstring& label) {
    auto owner = procinfo::ResolveWindowOwner(pid, processName);
    LOG_INFO("alerts", L"Test border for " << processName << L" PID " << pid << L": " << owner.windows.size()
                                           << L" window(s) (owner PID " << owner.pid << L")");
    if (owner.windows.empty()) {
        std::wstring msg = processName + L" (PID " + std::to_wstring(pid) + L") has no visible window to highlight.";
        MessageBoxW(m_alertsDialog && m_alertsDialog->Hwnd() ? m_alertsDialog->Hwnd() : m_hWnd, msg.c_str(), L"Window Alerts", MB_ICONINFORMATION);
        return;
    }
    m_overlays->Flash(owner.windows, style, label);
}

void SoundTrackerGUI::OnRulesChanged() {
    SaveSettings();
    m_tracker->SetRules(m_settings.alerts);
    LOG_INFO("ui", L"Alert rules saved (" << m_settings.alerts.size() << L")");
    UpdateStatusBar();
}

std::vector<LiveSessionInfo> SoundTrackerGUI::GetLiveSessions() { return m_tracker->GetLiveSessions(); }

void SoundTrackerGUI::SetDebugPreset(int preset) {
    m_settings.debugPreset = preset;
    dbg::SetLevel(dbg::LevelForPreset(static_cast<dbg::Preset>(preset)));
    LOG_INFO("ui", L"Debug log level set to " << dbg::PresetName(static_cast<dbg::Preset>(preset)));
    SaveSettings();
    UpdateMenuChecks();
    UpdateStatusBar();
}

void SoundTrackerGUI::ShowAbout() {
    std::wstring text = std::wstring(APP_NAME_WSTR) + L" " + APP_VERSION_WSTR +
                        L"\n\nData folder: " + AppPaths::DataDir() + (AppPaths::IsPortable() ? L" (portable)" : L"") +
                        L"\nSettings: " + AppPaths::SettingsFile() +
                        L"\nDebug log: " + dbg::CurrentLogPath() +
                        L"\nSound log: " + (m_tracker->GetCurrentLogPath().empty() ? L"(not tracking yet)" : m_tracker->GetCurrentLogPath()) +
                        L"\n\nTo report a problem, set Options > Debug Logging > Debug, reproduce it, then send the "
                        L"debug_*.log files from the logs folder.\n\nhttps://github.com/IronAdamant/windows_sound_tracker";
    MessageBoxW(m_hWnd, text.c_str(), L"About Sound Tracker", MB_ICONINFORMATION);
}

void SoundTrackerGUI::SaveSettings() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (m_hWnd && GetWindowPlacement(m_hWnd, &wp)) {
        m_settings.windowRect = wp.rcNormalPosition;
        m_settings.hasWindowRect = true;
    }
    if (m_hFilterEdit) {
        int len = GetWindowTextLengthW(m_hFilterEdit);
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(m_hFilterEdit, &text[0], len + 1);
        text.resize(static_cast<size_t>(len));
        m_settings.filterText = text;
    }
    if (!m_settings.Save(AppPaths::SettingsFile())) LOG_WARN("ui", L"Saving settings failed: " << AppPaths::SettingsFile());
}

void SoundTrackerGUI::MinimizeToTray() {
    if (!Shell_NotifyIconW(NIM_ADD, &m_trayIcon)) {
        LOG_WARN("ui", L"Could not add tray icon; staying minimized on the taskbar");
        return;
    }
    ShowWindow(m_hWnd, SW_HIDE);
    m_inTray = true;
}

void SoundTrackerGUI::RestoreFromTray() {
    ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
    if (m_inTray) Shell_NotifyIconW(NIM_DELETE, &m_trayIcon);
    m_inTray = false;
}

void SoundTrackerGUI::OnTrayIcon(LPARAM lParam) {
    switch (lParam) {
        case WM_LBUTTONDBLCLK: RestoreFromTray(); break;
        case WM_RBUTTONUP: ShowTrayMenu(); break;
    }
}

void SoundTrackerGUI::ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_RESTORE, L"Restore");
    AppendMenuW(menu, MF_STRING, ID_MENU_START_STOP, m_isTracking ? L"Stop Tracking" : L"Start Tracking");
    AppendMenuW(menu, MF_STRING, ID_MENU_ALERTS, L"Window Alerts...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");
    SetForegroundWindow(m_hWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);
}

int SoundTrackerGUI::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND alerts = m_alertsDialog ? m_alertsDialog->Hwnd() : nullptr;
        if (alerts && IsDialogMessageW(alerts, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
