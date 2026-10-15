#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include "Settings.h"
#include "SoundTracker.h"

// Modeless "Window Alerts" window: live per-session levels (for tuning
// thresholds) and the alert rule editor.
class AlertsDialog {
public:
    class Host {
    public:
        virtual ~Host() = default;
        virtual Settings& GetSettings() = 0;
        virtual void OnRulesChanged() = 0;  // Settings already updated: save and apply
        virtual std::vector<LiveSessionInfo> GetLiveSessions() = 0;
        virtual bool IsTracking() = 0;
        virtual void TestFlash(DWORD pid, const std::wstring& processName, const AlertRule& style, const std::wstring& label) = 0;
    };

    AlertsDialog(HINSTANCE instance, HWND owner, Host& host);
    ~AlertsDialog();

    void Show();
    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    HWND Label(const wchar_t* text, int x, int y, int w, int h = 20);
    HWND Edit(int id, int x, int y, int w);
    HWND Check(int id, const wchar_t* text, int x, int y, int w);
    HWND Button(int id, const wchar_t* text, int x, int y, int w, int h = 28);

    void RefreshLive();
    void RefreshRuleList();
    void SelectRule(int index);
    void LoadRuleToControls(const AlertRule& rule);
    bool ReadRuleFromControls(AlertRule& rule, bool showErrors);
    bool ConfirmDiscardChanges();
    void SaveCurrentRule();
    void OnLiveDispInfo(NMLVDISPINFOW* info);
    int SelectedLiveIndex() const;

    HINSTANCE m_instance;
    HWND m_owner;
    Host& m_host;
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    HWND m_liveList = nullptr;
    HWND m_liveHint = nullptr;
    HWND m_ruleList = nullptr;
    std::vector<LiveSessionInfo> m_live;
    int m_selectedRule = -1;
};
