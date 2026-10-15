#pragma once
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <memory>
#include <string>
#include <vector>

#include "AlertOverlay.h"
#include "AlertsDialog.h"
#include "Settings.h"
#include "SoundTracker.h"

// Window IDs
#define ID_BUTTON_START_STOP    1001
#define ID_BUTTON_ALERTS        1002
#define ID_BUTTON_CLEAR         1003
#define ID_LISTVIEW             1004
#define ID_STATUSBAR            1005
#define ID_TIMER_UPDATE         1006
#define ID_TRAY_ICON            1007
#define ID_CHECKBOX_FILTER      1008
#define ID_EDIT_FILTER          1009
#define ID_TIMER_EXIT           1010

// Menu IDs
#define ID_MENU_RESTORE         2001
#define ID_MENU_EXIT            2002
#define ID_MENU_START_STOP      2003
#define ID_MENU_OPEN_LOGS       2004
#define ID_MENU_CLEAR           2005
#define ID_MENU_ALERTS          2101
#define ID_MENU_AUTOSTART       2102
#define ID_MENU_WINDOW_TITLES   2103
#define ID_MENU_DEBUG_NORMAL    2110
#define ID_MENU_DEBUG_DEBUG     2111
#define ID_MENU_DEBUG_VERBOSE   2112
#define ID_MENU_OPEN_DEBUG_LOG  2201
#define ID_MENU_TESTING_GUIDE   2202
#define ID_MENU_ABOUT           2203

// Window messages
#define WM_TRAYICON  (WM_USER + 1)
#define WM_APP_ALERT (WM_APP + 1)

struct LaunchOptions {
    bool startTracking = false;
    int exitAfterSeconds = 0;  // Test automation: close after N seconds
};

class SoundTrackerGUI : public AlertsDialog::Host {
public:
    explicit SoundTrackerGUI(Settings& settings);
    ~SoundTrackerGUI() override;

    bool Initialize(HINSTANCE hInstance, const LaunchOptions& options);
    int Run();

    // AlertsDialog::Host
    Settings& GetSettings() override { return m_settings; }
    void OnRulesChanged() override;
    std::vector<LiveSessionInfo> GetLiveSessions() override;
    bool IsTracking() override { return m_isTracking; }
    void TestFlash(DWORD pid, const std::wstring& processName, const AlertRule& style, const std::wstring& label) override;

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StatusBarProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(HINSTANCE hInstance);
    void CreateMenuBar();
    void CreateControls();
    void CreateListView();
    void CreateStatusBar();
    void CreateTrayIcon();
    void LayoutControls();

    void UpdateListView(bool force);
    void UpdateStatusBar();
    void UpdateMenuChecks();
    void OnListDispInfo(NMLVDISPINFOW* info);
    std::wstring CellText(const AudioEvent& e, int column) const;

    void OnStartStop();
    void OnClear();
    void OnFilterChanged();
    void OnListViewClick();
    void OnStatusBarClick();
    void OnTrayIcon(LPARAM lParam);
    void OnAlert(AlertFire* fire);
    void SetDebugPreset(int preset);
    void OpenLogsFolder(const std::wstring& selectFile);
    void ShowAbout();
    void ShowTrayMenu();
    void MinimizeToTray();
    void RestoreFromTray();
    void SaveSettings();

    Settings& m_settings;
    HINSTANCE m_instance = nullptr;
    HWND m_hWnd = nullptr;
    HWND m_hListView = nullptr;
    HWND m_hButtonStartStop = nullptr;
    HWND m_hButtonAlerts = nullptr;
    HWND m_hButtonClear = nullptr;
    HWND m_hStatusBar = nullptr;
    HWND m_hFilterCheckbox = nullptr;
    HWND m_hFilterEdit = nullptr;
    HMENU m_hMenu = nullptr;
    HFONT m_hFont = nullptr;
    HFONT m_hBoldFont = nullptr;
    HICON m_hIcon = nullptr;
    HICON m_hIconSmall = nullptr;

    NOTIFYICONDATAW m_trayIcon{};
    bool m_inTray = false;

    std::unique_ptr<SoundTracker> m_tracker;
    std::unique_ptr<AlertOverlayManager> m_overlays;
    std::unique_ptr<AlertsDialog> m_alertsDialog;
    bool m_isTracking = false;

    std::vector<AudioEvent> m_rows;  // Newest first, filtered
    uint64_t m_lastVersion = ~0ull;
    uint64_t m_clearedAfterId = 0;
    bool m_exitTimerFired = false;
    WNDPROC m_originalStatusProc = nullptr;
};
