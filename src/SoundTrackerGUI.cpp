#include "../include/SoundTrackerGUI.h"
#include "../include/Logger.h"
#include "../resource.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

SoundTrackerGUI::SoundTrackerGUI() 
    : m_hWnd(nullptr), m_hListView(nullptr), m_hButtonStartStop(nullptr),
      m_hButtonClear(nullptr), m_hStatusBar(nullptr),
      m_hFilterCheckbox(nullptr), m_hFilterEdit(nullptr), m_hFont(nullptr),
      m_hBoldFont(nullptr), m_hIcon(nullptr), m_trayIcon({}), m_inTray(false),
      m_isTracking(false), m_updateThreadRunning(false),
      m_lastEventCount(0), m_filterEnabled(false), m_originalStatusProc(nullptr) {
    m_tracker = std::make_unique<SoundTracker>();
    m_lastUpdateTime = std::chrono::system_clock::now();
}

SoundTrackerGUI::~SoundTrackerGUI() {
    Cleanup();
}

bool SoundTrackerGUI::Initialize(HINSTANCE hInstance) {
    // Initialize common controls
    INITCOMMONCONTROLSEX icex = { 0 };
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);
    
    // Initialize sound tracker
    if (!m_tracker->Initialize()) {
        MessageBox(nullptr, L"Failed to initialize audio monitoring system.", 
                  L"Error", MB_ICONERROR);
        return false;
    }
    
    // Create main window
    if (!CreateMainWindow(hInstance)) {
        return false;
    }
    
    // Start update thread
    m_updateThreadRunning = true;
    m_updateThread = std::thread(&SoundTrackerGUI::UpdateThreadProc, this);
    
    return true;
}

bool SoundTrackerGUI::CreateMainWindow(HINSTANCE hInstance) {
    // Register window class
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoundTrackerWindow";
    wc.hIconSm = wc.hIcon;
    
    if (!RegisterClassEx(&wc)) {
        return false;
    }
    
    // Create window
    m_hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        L"SoundTrackerWindow",
        L"Windows 11 Sound Tracker - Administrator",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1200, 700,
        nullptr, nullptr,
        hInstance, this
    );
    
    if (!m_hWnd) {
        return false;
    }
    
    // Create fonts
    m_hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    
    m_hBoldFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    
    CreateControls();
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);
    
    return true;
}

void SoundTrackerGUI::CreateControls() {
    // Create toolbar area
    int buttonY = 10;
    int buttonHeight = 35;
    int buttonSpacing = 10;
    
    // Start/Stop button
    m_hButtonStartStop = CreateWindow(
        L"BUTTON", L"Start Tracking",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, buttonY, 150, buttonHeight,
        m_hWnd, (HMENU)ID_BUTTON_START_STOP,
        GetModuleHandle(nullptr), nullptr
    );
    SendMessage(m_hButtonStartStop, WM_SETFONT, (WPARAM)m_hBoldFont, TRUE);
    
    // Clear button (moved to where export was)
    m_hButtonClear = CreateWindow(
        L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        170, buttonY, 80, buttonHeight,
        m_hWnd, (HMENU)ID_BUTTON_CLEAR,
        GetModuleHandle(nullptr), nullptr
    );
    SendMessage(m_hButtonClear, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    
    // Filter checkbox
    m_hFilterCheckbox = CreateWindow(
        L"BUTTON", L"Filter:",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        260, buttonY + 5, 70, 25,
        m_hWnd, (HMENU)ID_CHECKBOX_FILTER,
        GetModuleHandle(nullptr), nullptr
    );
    SendMessage(m_hFilterCheckbox, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    
    // Filter edit box
    m_hFilterEdit = CreateWindow(
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        340, buttonY + 3, 200, 28,
        m_hWnd, (HMENU)ID_EDIT_FILTER,
        GetModuleHandle(nullptr), nullptr
    );
    SendMessage(m_hFilterEdit, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    EnableWindow(m_hFilterEdit, FALSE);
    
    // Create ListView
    CreateListView();
    
    // Create Status Bar
    CreateStatusBar();
    
    // Create Tray Icon
    CreateTrayIcon();
}

void SoundTrackerGUI::CreateListView() {
    m_hListView = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | 
        LVS_SHOWSELALWAYS | LVS_AUTOARRANGE,
        10, 60, 0, 0,  // Size will be set in WM_SIZE
        m_hWnd, (HMENU)ID_LISTVIEW,
        GetModuleHandle(nullptr), nullptr
    );
    
    // Set extended styles
    ListView_SetExtendedListViewStyle(m_hListView, 
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    
    // Initialize columns
    InitializeListViewColumns();
    
    // Set font
    SendMessage(m_hListView, WM_SETFONT, (WPARAM)m_hFont, TRUE);
}

void SoundTrackerGUI::InitializeListViewColumns() {
    LVCOLUMN lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    
    // Time column
    lvc.pszText = (LPWSTR)L"Time";
    lvc.cx = 100;
    lvc.iSubItem = 0;
    ListView_InsertColumn(m_hListView, 0, &lvc);
    
    // Count column (for batched events)
    lvc.pszText = (LPWSTR)L"Count";
    lvc.cx = 50;
    lvc.iSubItem = 1;
    ListView_InsertColumn(m_hListView, 1, &lvc);
    
    // Process column
    lvc.pszText = (LPWSTR)L"Process";
    lvc.cx = 150;
    lvc.iSubItem = 2;
    ListView_InsertColumn(m_hListView, 2, &lvc);
    
    // PID column
    lvc.pszText = (LPWSTR)L"PID";
    lvc.cx = 60;
    lvc.iSubItem = 3;
    ListView_InsertColumn(m_hListView, 3, &lvc);
    
    // Description column
    lvc.pszText = (LPWSTR)L"Description";
    lvc.cx = 250;
    lvc.iSubItem = 4;
    ListView_InsertColumn(m_hListView, 4, &lvc);
    
    // Volume column
    lvc.pszText = (LPWSTR)L"Volume";
    lvc.cx = 80;
    lvc.iSubItem = 5;
    ListView_InsertColumn(m_hListView, 5, &lvc);
    
    // Peak column
    lvc.pszText = (LPWSTR)L"Peak";
    lvc.cx = 80;
    lvc.iSubItem = 6;
    ListView_InsertColumn(m_hListView, 6, &lvc);
    
    // Path column
    lvc.pszText = (LPWSTR)L"Path";
    lvc.cx = 350;
    lvc.iSubItem = 7;
    ListView_InsertColumn(m_hListView, 7, &lvc);
}

void SoundTrackerGUI::CreateStatusBar() {
    m_hStatusBar = CreateWindowEx(
        0, STATUSCLASSNAME, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        m_hWnd, (HMENU)ID_STATUSBAR,
        GetModuleHandle(nullptr), nullptr
    );
    
    // Set parts
    int parts[] = { 200, 400, 600, -1 };
    SendMessage(m_hStatusBar, SB_SETPARTS, 4, (LPARAM)parts);
    
    // Initial text
    SendMessage(m_hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Status: Ready");
    SendMessage(m_hStatusBar, SB_SETTEXT, 1, (LPARAM)L"Events: 0");
    SendMessage(m_hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Duration: 00:00:00");
    SendMessage(m_hStatusBar, SB_SETTEXT, 3, (LPARAM)L"Logs saved to: logs\\");
    
    // Subclass the status bar to handle clicks
    SetWindowLongPtr(m_hStatusBar, GWLP_USERDATA, (LONG_PTR)this);
    m_originalStatusProc = (WNDPROC)SetWindowLongPtr(m_hStatusBar, GWLP_WNDPROC, (LONG_PTR)StatusBarProc);
}

void SoundTrackerGUI::CreateTrayIcon() {
    m_trayIcon.cbSize = sizeof(NOTIFYICONDATA);
    m_trayIcon.hWnd = m_hWnd;
    m_trayIcon.uID = ID_TRAY_ICON;
    m_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_trayIcon.uCallbackMessage = WM_TRAYICON;
    m_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(m_trayIcon.szTip, L"Sound Tracker - Click to restore");
}

LRESULT CALLBACK SoundTrackerGUI::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SoundTrackerGUI* pThis = nullptr;
    
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = reinterpret_cast<SoundTrackerGUI*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<SoundTrackerGUI*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    }
    
    if (pThis) {
        return pThis->HandleMessage(uMsg, wParam, lParam);
    }
    
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK SoundTrackerGUI::StatusBarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SoundTrackerGUI* pThis = (SoundTrackerGUI*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    
    if (pThis && uMsg == WM_LBUTTONUP) {
        // Get click position
        int xPos = GET_X_LPARAM(lParam);
        
        // Get status bar part boundaries
        int parts[4];
        SendMessage(hWnd, SB_GETPARTS, 4, (LPARAM)parts);
        
        // Check if clicked in the 4th part (log path section)
        if (xPos > parts[2] && !pThis->m_isTracking) {
            pThis->OnStatusBarClick();
            return 0;
        }
    }
    
    // Call original procedure
    if (pThis && pThis->m_originalStatusProc) {
        return CallWindowProc(pThis->m_originalStatusProc, hWnd, uMsg, wParam, lParam);
    }
    
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

LRESULT SoundTrackerGUI::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BUTTON_START_STOP:
                    OnStartStop();
                    break;
                case ID_BUTTON_CLEAR:
                    OnClear();
                    break;
                case ID_CHECKBOX_FILTER:
                    OnFilterChanged();
                    break;
                case ID_MENU_RESTORE:
                    RestoreFromTray();
                    break;
                case ID_MENU_EXIT:
                    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
                    break;
                case ID_MENU_START_STOP:
                    OnStartStop();
                    break;
            }
            break;
            
        case WM_NOTIFY:
            {
                LPNMHDR pnmh = (LPNMHDR)lParam;
                if (pnmh->idFrom == ID_LISTVIEW && pnmh->code == NM_CLICK) {
                    OnListViewClick();
                }
            }
            break;
            
            
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                MinimizeToTray();
            } else {
                // Resize ListView
                RECT rcClient;
                GetClientRect(m_hWnd, &rcClient);
                SetWindowPos(m_hListView, nullptr, 
                           10, 60, 
                           rcClient.right - 20, 
                           rcClient.bottom - 110,
                           SWP_NOZORDER);
                
                // Resize status bar
                SendMessage(m_hStatusBar, WM_SIZE, 0, 0);
            }
            break;
            
        case WM_TRAYICON:
            OnTrayIcon(lParam);
            break;
            
        case WM_TIMER:
            if (wParam == ID_TIMER_UPDATE) {
                UpdateListView();
                UpdateStatusBar();
            }
            break;
            
        case WM_CLOSE:
            if (m_isTracking) {
                int result = MessageBox(m_hWnd, 
                    L"Sound tracking is active. Do you want to stop and exit?",
                    L"Confirm Exit", MB_YESNO | MB_ICONQUESTION);
                if (result != IDYES) {
                    return 0;
                }
                // Stop tracking before exit
                m_tracker->Stop();
                m_isTracking = false;
            }
            DestroyWindow(m_hWnd);
            break;
            
        case WM_DESTROY:
            if (m_inTray) {
                Shell_NotifyIcon(NIM_DELETE, &m_trayIcon);
            }
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(m_hWnd, uMsg, wParam, lParam);
    }
    
    return 0;
}

void SoundTrackerGUI::OnStartStop() {
    if (m_isTracking) {
        // Stop tracking and auto-save
        m_tracker->Stop();
        m_isTracking = false;
        SetWindowText(m_hButtonStartStop, L"Start Tracking");
        SendMessage(m_hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Status: Stopped");
    } else {
        // Start tracking
        m_tracker->Start();
        m_isTracking = true;
        SetWindowText(m_hButtonStartStop, L"Stop Tracking");
        SendMessage(m_hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Status: Recording");
    }
}

void SoundTrackerGUI::OnClear() {
    ClearListView();
    m_lastEventCount = m_tracker->GetEventCount();
}

void SoundTrackerGUI::OnFilterChanged() {
    m_filterEnabled = (SendMessage(m_hFilterCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(m_hFilterEdit, m_filterEnabled);
    
    if (m_filterEnabled) {
        SetFocus(m_hFilterEdit);
    }
    
    UpdateListView();
}

void SoundTrackerGUI::OnListViewClick() {
    // Get the selected item
    int selectedIndex = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (selectedIndex == -1) return;
    
    // Build the row content
    std::wstring rowContent;
    WCHAR buffer[512];
    
    // Get all column data
    for (int col = 0; col < 8; col++) {
        ListView_GetItemText(m_hListView, selectedIndex, col, buffer, 512);
        if (col > 0) rowContent += L"\t";
        rowContent += buffer;
    }
    
    // Copy to clipboard
    if (OpenClipboard(m_hWnd)) {
        EmptyClipboard();
        
        size_t size = (rowContent.length() + 1) * sizeof(WCHAR);
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hGlobal) {
            LPWSTR pGlobal = (LPWSTR)GlobalLock(hGlobal);
            if (pGlobal) {
                wcscpy_s(pGlobal, rowContent.length() + 1, rowContent.c_str());
                GlobalUnlock(hGlobal);
                SetClipboardData(CF_UNICODETEXT, hGlobal);
            }
        }
        
        CloseClipboard();
        
        // Brief visual feedback in status bar
        SendMessage(m_hStatusBar, SB_SETTEXT, 3, (LPARAM)L"Row copied to clipboard!");
        
        // The status will be updated on next timer tick
    }
}

void SoundTrackerGUI::OnStatusBarClick() {
    // Get the current log path from the tracker
    std::wstring logPath = m_tracker->GetCurrentLogPath();
    
    if (!logPath.empty()) {
        // Get the directory path (remove filename)
        size_t lastSlash = logPath.find_last_of(L"\\");
        std::wstring logDir = logPath.substr(0, lastSlash);
        
        // Convert to absolute path if needed
        WCHAR fullPath[MAX_PATH];
        GetFullPathNameW(logDir.c_str(), MAX_PATH, fullPath, nullptr);
        
        // Open Windows Explorer with the log file selected
        std::wstring explorerCmd = L"/select,\"" + logPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", explorerCmd.c_str(), nullptr, SW_SHOW);
    }
}

void SoundTrackerGUI::UpdateListView() {
    // Get current events
    auto now = std::chrono::system_clock::now();
    auto thirtySecondsAgo = now - std::chrono::seconds(30);
    auto events = m_tracker->GetEvents(thirtySecondsAgo, now);
    
    // Apply filter if enabled
    if (m_filterEnabled) {
        WCHAR filterText[256];
        GetWindowText(m_hFilterEdit, filterText, 256);
        m_filterText = filterText;
        
        // Convert to lowercase for case-insensitive search
        std::transform(m_filterText.begin(), m_filterText.end(), m_filterText.begin(), ::towlower);
        
        events.erase(std::remove_if(events.begin(), events.end(),
            [this](const AudioEvent& event) {
                std::wstring processLower = event.processName;
                std::transform(processLower.begin(), processLower.end(), processLower.begin(), ::towlower);
                
                std::wstring descLower = event.soundDescription;
                std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);
                
                return processLower.find(m_filterText) == std::wstring::npos &&
                       descLower.find(m_filterText) == std::wstring::npos;
            }), events.end());
    }
    
    // Only update if there are new events - compare actual content, not just count
    static size_t lastEventHash = 0;
    size_t currentHash = 0;
    for (const auto& event : events) {
        currentHash ^= std::hash<DWORD>()(event.processId) + 
                      std::hash<std::wstring>()(event.processName);
    }
    
    if (currentHash != lastEventHash || events.size() != ListView_GetItemCount(m_hListView)) {
        lastEventHash = currentHash;
        
        // Use SetRedraw to prevent flashing
        SendMessage(m_hListView, WM_SETREDRAW, FALSE, 0);
        
        ClearListView();
        
        // Add events in reverse order (newest first)
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            AddEventToListView(*it);
        }
        
        // Re-enable drawing and refresh
        SendMessage(m_hListView, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(m_hListView, NULL, FALSE);
    }
}

void SoundTrackerGUI::AddEventToListView(const AudioEvent& event) {
    LVITEM lvi = { 0 };
    lvi.mask = LVIF_TEXT;
    lvi.iItem = 0;  // Insert at top
    
    // Format time with only hours and minutes
    auto time_t = std::chrono::system_clock::to_time_t(event.timestamp);
    std::tm tm;
    localtime_s(&tm, &time_t);
    WCHAR timeStr[32];
    swprintf_s(timeStr, L"%02d:%02d", tm.tm_hour, tm.tm_min);
    
    // Add item
    lvi.pszText = timeStr;
    int index = ListView_InsertItem(m_hListView, &lvi);
    
    // Set subitems
    // Count column - only show if > 1
    if (event.eventCount > 1) {
        WCHAR countStr[16];
        swprintf_s(countStr, L"%d", event.eventCount);
        ListView_SetItemText(m_hListView, index, 1, countStr);
    } else {
        ListView_SetItemText(m_hListView, index, 1, L"");
    }
    
    ListView_SetItemText(m_hListView, index, 2, (LPWSTR)event.processName.c_str());
    
    WCHAR pidStr[16];
    swprintf_s(pidStr, L"%d", event.processId);
    ListView_SetItemText(m_hListView, index, 3, pidStr);
    
    ListView_SetItemText(m_hListView, index, 4, (LPWSTR)event.soundDescription.c_str());
    
    WCHAR volumeStr[16];
    swprintf_s(volumeStr, L"%.0f%%", event.volumeLevel * 100);
    ListView_SetItemText(m_hListView, index, 5, volumeStr);
    
    WCHAR peakStr[16];
    swprintf_s(peakStr, L"%.0f%%", event.peakLevel * 100);
    ListView_SetItemText(m_hListView, index, 6, peakStr);
    
    ListView_SetItemText(m_hListView, index, 7, (LPWSTR)event.processPath.c_str());
}

void SoundTrackerGUI::ClearListView() {
    ListView_DeleteAllItems(m_hListView);
}

void SoundTrackerGUI::UpdateStatusBar() {
    // Update event count
    size_t eventCount = m_tracker->GetEventCount();
    WCHAR countStr[64];
    swprintf_s(countStr, L"Events: %zu", eventCount);
    SendMessage(m_hStatusBar, SB_SETTEXT, 1, (LPARAM)countStr);
    
    // Update duration
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_tracker->GetStartTime());
    
    int hours = static_cast<int>(duration.count() / 3600);
    int minutes = static_cast<int>((duration.count() % 3600) / 60);
    int seconds = static_cast<int>(duration.count() % 60);
    
    WCHAR durationStr[64];
    swprintf_s(durationStr, L"Duration: %02d:%02d:%02d", hours, minutes, seconds);
    SendMessage(m_hStatusBar, SB_SETTEXT, 2, (LPARAM)durationStr);
    
    // Show current log file path
    std::wstring logPath = m_tracker->GetCurrentLogPath();
    if (!logPath.empty()) {
        std::wstring statusText;
        if (!m_isTracking) {
            // When stopped, show it's clickable
            statusText = L"Log: " + logPath + L" (Click to open)";
        } else {
            statusText = L"Log: " + logPath;
        }
        SendMessage(m_hStatusBar, SB_SETTEXT, 3, (LPARAM)statusText.c_str());
    }
}

void SoundTrackerGUI::MinimizeToTray() {
    Shell_NotifyIcon(NIM_ADD, &m_trayIcon);
    ShowWindow(m_hWnd, SW_HIDE);
    m_inTray = true;
}

void SoundTrackerGUI::RestoreFromTray() {
    ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
    Shell_NotifyIcon(NIM_DELETE, &m_trayIcon);
    m_inTray = false;
}

void SoundTrackerGUI::OnTrayIcon(LPARAM lParam) {
    switch (lParam) {
        case WM_LBUTTONDBLCLK:
            RestoreFromTray();
            break;
            
        case WM_RBUTTONUP:
            ShowTrayMenu();
            break;
    }
}

void SoundTrackerGUI::ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, ID_MENU_RESTORE, L"Restore");
    AppendMenu(hMenu, MF_STRING, ID_MENU_START_STOP, 
               m_isTracking ? L"Stop Tracking" : L"Start Tracking");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, ID_MENU_EXIT, L"Exit");
    
    SetForegroundWindow(m_hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(hMenu);
}

void SoundTrackerGUI::UpdateThreadProc() {
    while (m_updateThreadRunning) {
        if (m_isTracking) {
            // Post message to update UI in main thread
            PostMessage(m_hWnd, WM_TIMER, ID_TIMER_UPDATE, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Faster updates
    }
}



void SoundTrackerGUI::UpdateButtonStates() {
    // Update button states based on tracking status
    EnableWindow(m_hButtonClear, !m_isTracking);
}

std::wstring SoundTrackerGUI::FormatDuration(DWORD milliseconds) {
    DWORD seconds = milliseconds / 1000;
    DWORD ms = milliseconds % 1000;
    return std::to_wstring(seconds) + L"." + std::to_wstring(ms) + L"s";
}

std::wstring SoundTrackerGUI::FormatVolume(float level) {
    int percentage = static_cast<int>(level * 100);
    return std::to_wstring(percentage) + L"%";
}

void SoundTrackerGUI::Cleanup() {
    // Stop tracking
    if (m_isTracking) {
        m_tracker->Stop();
    }
    
    // Stop update thread
    m_updateThreadRunning = false;
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }
    
    // Clean up fonts
    if (m_hFont) DeleteObject(m_hFont);
    if (m_hBoldFont) DeleteObject(m_hBoldFont);
}

int SoundTrackerGUI::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}