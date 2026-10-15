#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "Settings.h"

// Shows alerts on other applications' windows: a click-through, always-on-top
// coloured border drawn over the target window, plus an optional taskbar flash.
// Never touches the target process: the border is our own transparent window.
// Must be used from the GUI thread.
class AlertOverlayManager {
public:
    explicit AlertOverlayManager(HINSTANCE instance);
    ~AlertOverlayManager();
    AlertOverlayManager(const AlertOverlayManager&) = delete;
    AlertOverlayManager& operator=(const AlertOverlayManager&) = delete;

    void Flash(const std::vector<HWND>& targets, const AlertRule& style, const std::wstring& label);
    void CloseAll();

private:
    struct Overlay;
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowBorder(HWND target, const AlertRule& style, const std::wstring& label);

    HINSTANCE m_instance;
    bool m_classRegistered = false;
    std::unordered_map<HWND, HWND> m_overlayByTarget;
};
