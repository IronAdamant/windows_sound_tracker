#pragma once
#include "SoundTracker.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <atomic>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// Window IDs
#define ID_BUTTON_START_STOP    1001
#define ID_BUTTON_EXPORT        1002
#define ID_BUTTON_CLEAR         1003
#define ID_LISTVIEW             1004
#define ID_STATUSBAR            1005
#define ID_TIMER_UPDATE         1006
#define ID_TRAY_ICON           1007
#define ID_MENU_RESTORE        2001
#define ID_MENU_EXIT           2002
#define ID_MENU_START_STOP     2003
#define ID_CHECKBOX_FILTER     1008
#define ID_EDIT_FILTER         1009

// Window messages
#define WM_TRAYICON (WM_USER + 1)

class SoundTrackerGUI {
private:
    // Window handles
    HWND m_hWnd;
    HWND m_hListView;
    HWND m_hButtonStartStop;
    HWND m_hButtonClear;
    HWND m_hStatusBar;
    HWND m_hFilterCheckbox;
    HWND m_hFilterEdit;
    HFONT m_hFont;
    HFONT m_hBoldFont;
    HICON m_hIcon;
    
    // Tray icon
    NOTIFYICONDATA m_trayIcon;
    bool m_inTray;
    
    // Sound tracker
    std::unique_ptr<SoundTracker> m_tracker;
    std::atomic<bool> m_isTracking;
    std::thread m_updateThread;
    std::atomic<bool> m_updateThreadRunning;
    
    // GUI state
    int m_lastEventCount;
    std::chrono::system_clock::time_point m_lastUpdateTime;
    bool m_filterEnabled;
    std::wstring m_filterText;
    
    // Window procedures
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    // GUI creation
    bool CreateMainWindow(HINSTANCE hInstance);
    void CreateControls();
    void CreateListView();
    void CreateStatusBar();
    void CreateTrayIcon();
    void InitializeListViewColumns();
    
    // GUI updates
    void UpdateListView();
    void UpdateStatusBar();
    void UpdateButtonStates();
    void AddEventToListView(const AudioEvent& event);
    void ClearListView();
    std::wstring FormatDuration(DWORD milliseconds);
    std::wstring FormatVolume(float level);
    
    // Event handlers
    void OnStartStop();
    void OnClear();
    void OnFilterChanged();
    void OnListViewClick();
    void OnTrayIcon(LPARAM lParam);
    void ShowTrayMenu();
    void MinimizeToTray();
    void RestoreFromTray();
    
    // Update thread
    void UpdateThreadProc();
    
public:
    SoundTrackerGUI();
    ~SoundTrackerGUI();
    
    bool Initialize(HINSTANCE hInstance);
    int Run();
    void Cleanup();
};