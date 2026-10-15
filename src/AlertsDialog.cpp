#include "AlertsDialog.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

#include "DebugLog.h"

namespace {

const wchar_t* kClassName = L"SoundTrackerAlertsWindow";
const UINT_PTR kLiveTimer = 1;

enum : int {
    IDC_LIVE = 3001, IDC_TEST, IDC_FROM_SELECTED, IDC_IDENTIFY,
    IDC_RULES = 3010, IDC_ADD, IDC_REMOVE, IDC_PRESET,
    IDC_NAME = 3020, IDC_ENABLED, IDC_PATTERN, IDC_THRESHOLD, IDC_QUIET, IDC_MINLOUD, IDC_COOLDOWN, IDC_SUPPRESS, IDC_SCOPE,
    IDC_BORDER = 3030, IDC_COLOR, IDC_PICK, IDC_THICKNESS, IDC_DURATION, IDC_BLINK, IDC_LABEL, IDC_TASKBAR,
    IDC_SAVE = 3040,
};

std::wstring GetText(HWND parent, int id) {
    HWND h = GetDlgItem(parent, id);
    int len = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

void SetText(HWND parent, int id, const std::wstring& text) { SetWindowTextW(GetDlgItem(parent, id), text.c_str()); }

bool ParseInt(const std::wstring& s, int lo, int hi, int& out) {
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    while (end && *end == L' ') ++end;
    if (s.empty() || !end || *end) return false;
    if (v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool ParseFloat(const std::wstring& s, float lo, float hi, float& out) {
    wchar_t* end = nullptr;
    float v = wcstof(s.c_str(), &end);
    while (end && (*end == L' ' || *end == L'%')) ++end;
    if (s.empty() || !end || *end) return false;
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

std::wstring ColorHex(COLORREF c) {
    wchar_t buf[8];
    swprintf_s(buf, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

bool ParseColor(const std::wstring& text, COLORREF& out) {
    std::wstring h = text;
    if (!h.empty() && h[0] == L'#') h.erase(0, 1);
    if (h.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(h.c_str(), &end, 16);
    if (!end || *end) return false;
    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

std::wstring Percent(float v) {
    wchar_t buf[16];
    swprintf_s(buf, L"%.1f%%", v * 100.0f);
    return buf;
}

std::wstring Bar(float v) {
    const int width = 24;
    int filled = static_cast<int>(std::min(1.0f, std::max(0.0f, v)) * width + 0.5f);
    std::wstring bar(static_cast<size_t>(filled), L'\x2588');
    bar.append(static_cast<size_t>(width - filled), L'\x2591');
    return bar;
}

}  // namespace

AlertsDialog::AlertsDialog(HINSTANCE instance, HWND owner, Host& host) : m_instance(instance), m_owner(owner), m_host(host) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = reinterpret_cast<HICON>(SendMessageW(owner, WM_GETICON, ICON_BIG, 0));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    m_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

AlertsDialog::~AlertsDialog() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_font) DeleteObject(m_font);
}

void AlertsDialog::Show() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);
        return;
    }
    RECT rc{0, 0, 990, 680};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE, 0);
    m_hwnd = CreateWindowExW(0, kClassName, L"Window Alerts - Sound Tracker",
                             WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, m_owner, nullptr, m_instance, this);
    if (!m_hwnd) {
        LOG_ERROR("ui", L"Cannot create alerts window: " << dbg::Win32ErrorToString(GetLastError()));
        return;
    }
    CreateControls();
    RefreshRuleList();
    SelectRule(m_host.GetSettings().alerts.empty() ? -1 : 0);
    RefreshLive();
    SetTimer(m_hwnd, kLiveTimer, 100, nullptr);
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    LOG_DEBUG("ui", L"Alerts window opened");
}

HWND AlertsDialog::Label(const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y + 4, w, h, m_hwnd, nullptr, m_instance, nullptr);
}

HWND AlertsDialog::Edit(int id, int x, int y, int w) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 26,
                           m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
}

HWND AlertsDialog::Check(int id, const wchar_t* text, int x, int y, int w) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 26, m_hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
}

HWND AlertsDialog::Button(int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, m_hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
}

void AlertsDialog::CreateControls() {
    m_liveHint = Label(L"", 10, 2, 970, 40);

    m_liveList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
                                 10, 46, 970, 210, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIVE)), m_instance, nullptr);
    ListView_SetExtendedListViewStyle(m_liveList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    struct Col { const wchar_t* name; int width; };
    const Col cols[] = {{L"Process", 130}, {L"PID", 55}, {L"Window", 180}, {L"Now", 55}, {L"Recent max", 85},
                        {L"Level", 175}, {L"Alert rule", 110}, {L"Alert state", 110}, {L"Alerts", 50}};
    for (int i = 0; i < static_cast<int>(sizeof(cols) / sizeof(cols[0])); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<LPWSTR>(cols[i].name);
        c.cx = cols[i].width;
        c.iSubItem = i;
        ListView_InsertColumn(m_liveList, i, &c);
    }

    Button(IDC_TEST, L"Test border on selected window", 10, 262, 240);
    Button(IDC_FROM_SELECTED, L"New rule from selected", 258, 262, 190);
    Button(IDC_IDENTIFY, L"Identify all windows", 456, 262, 170);

    Label(L"Alert rules", 10, 300, 230);
    m_ruleList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 10,
                                 326, 230, 270, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RULES)), m_instance, nullptr);
    Button(IDC_ADD, L"Add", 10, 604, 60);
    Button(IDC_REMOVE, L"Remove", 75, 604, 70);
    Button(IDC_PRESET, L"WoW preset", 150, 604, 90);

    const int lx = 255, ex = 430, row = 32;
    int y = 326;
    Label(L"Rule name", lx, y, 170);
    Edit(IDC_NAME, ex, y, 290);
    y += row;
    Check(IDC_ENABLED, L"Enabled", lx, y, 170);
    y += row;
    Label(L"Process name(s)", lx, y, 170);
    Edit(IDC_PATTERN, ex, y, 290);
    y += row;
    Label(L"Trigger level (%)", lx, y, 170);
    Edit(IDC_THRESHOLD, ex, y, 80);
    y += row;
    Label(L"Quiet before (ms)", lx, y, 170);
    Edit(IDC_QUIET, ex, y, 80);
    y += row;
    Label(L"Must stay loud (ms)", lx, y, 170);
    Edit(IDC_MINLOUD, ex, y, 80);
    y += row;
    Label(L"Cooldown (ms)", lx, y, 170);
    Edit(IDC_COOLDOWN, ex, y, 80);
    y += row;
    Label(L"Ignore after input (ms)", lx, y, 170);
    Edit(IDC_SUPPRESS, ex, y, 80);
    HWND scope = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, ex + 90, y, 200, 200,
                                 m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SCOPE)), m_instance, nullptr);
    SendMessageW(scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"...in the window I'm using"));
    SendMessageW(scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"...in all windows"));

    const int cx = 740;
    y = 326 + row;
    Check(IDC_BORDER, L"Show border on the window", cx, y, 240);
    y += row;
    Label(L"Colour", cx, y, 60);
    Edit(IDC_COLOR, cx + 60, y, 80);
    Button(IDC_PICK, L"Pick...", cx + 148, y - 1, 70);
    y += row;
    Label(L"Thickness (px)", cx, y, 110);
    Edit(IDC_THICKNESS, cx + 120, y, 60);
    y += row;
    Label(L"Duration (ms)", cx, y, 110);
    Edit(IDC_DURATION, cx + 120, y, 60);
    y += row;
    Check(IDC_BLINK, L"Blink", cx, y, 80);
    Check(IDC_LABEL, L"Show name label", cx + 85, y, 150);
    y += row;
    Check(IDC_TASKBAR, L"Flash taskbar until focused", cx, y, 240);

    Button(IDC_SAVE, L"Save rule", lx, 600, 120, 32);
    Label(L"Watch 'Recent max' while the sound you want plays, then set the trigger level below it but above "
          L"everything else that window plays. 'Ignore after input' skips sounds right after your own key press or click "
          L"(e.g. the fishing cast).",
          lx + 130, 590, 590, 80);

    EnumChildWindows(
        m_hwnd,
        [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(m_font));
}

void AlertsDialog::RefreshLive() {
    bool tracking = m_host.IsTracking();
    int selected = SelectedLiveIndex();
    DWORD selectedPid = selected >= 0 && selected < static_cast<int>(m_live.size()) ? m_live[selected].processId : 0;
    m_live = tracking ? m_host.GetLiveSessions() : std::vector<LiveSessionInfo>();
    SetWindowTextW(m_liveHint, tracking
                                   ? L"Live audio sessions (one row per process per output device). Levels update 10x per second."
                                   : L"Not tracking. Click 'Start Tracking' in the main window to see live audio sessions and use alerts.");
    ListView_SetItemCountEx(m_liveList, static_cast<int>(m_live.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    if (selectedPid) {
        for (size_t i = 0; i < m_live.size(); ++i) {
            if (m_live[i].processId == selectedPid) {
                if (static_cast<int>(i) != selected) {
                    ListView_SetItemState(m_liveList, -1, 0, LVIS_SELECTED);
                    ListView_SetItemState(m_liveList, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
                }
                break;
            }
        }
    }
    InvalidateRect(m_liveList, nullptr, FALSE);
}

int AlertsDialog::SelectedLiveIndex() const {
    return m_liveList ? ListView_GetNextItem(m_liveList, -1, LVNI_SELECTED) : -1;
}

void AlertsDialog::OnLiveDispInfo(NMLVDISPINFOW* info) {
    if (!(info->item.mask & LVIF_TEXT)) return;
    int row = info->item.iItem;
    if (row < 0 || row >= static_cast<int>(m_live.size())) return;
    const LiveSessionInfo& s = m_live[row];
    std::wstring text;
    switch (info->item.iSubItem) {
        case 0: text = s.processName; break;
        case 1: text = std::to_wstring(s.processId); break;
        case 2: text = s.isSystem ? L"(system sounds)" : s.windowTitle; break;
        case 3: text = Percent(s.peakNow); break;
        case 4: text = Percent(s.peakRecentMax); break;
        case 5: text = Bar(s.peakNow); break;
        case 6: text = s.ruleNames; break;
        case 7: text = s.alertState; break;
        case 8: text = s.ruleNames.empty() ? L"" : std::to_wstring(s.alertsFired); break;
    }
    wcsncpy_s(info->item.pszText, static_cast<size_t>(info->item.cchTextMax), text.c_str(), _TRUNCATE);
}

void AlertsDialog::RefreshRuleList() {
    SendMessageW(m_ruleList, LB_RESETCONTENT, 0, 0);
    for (const auto& rule : m_host.GetSettings().alerts) {
        std::wstring entry = (rule.enabled ? L"" : L"[off] ") + rule.name;
        SendMessageW(m_ruleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
    }
}

void AlertsDialog::SelectRule(int index) {
    auto& rules = m_host.GetSettings().alerts;
    if (index >= static_cast<int>(rules.size())) index = static_cast<int>(rules.size()) - 1;
    m_selectedRule = index;
    SendMessageW(m_ruleList, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    LoadRuleToControls(index >= 0 ? rules[static_cast<size_t>(index)] : AlertRule());
}

void AlertsDialog::LoadRuleToControls(const AlertRule& r) {
    auto fmt = [](float v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%g", v);
        return std::wstring(buf);
    };
    SetText(m_hwnd, IDC_NAME, r.name);
    Button_SetCheck(GetDlgItem(m_hwnd, IDC_ENABLED), r.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetText(m_hwnd, IDC_PATTERN, r.processPattern);
    SetText(m_hwnd, IDC_THRESHOLD, fmt(r.thresholdPercent));
    SetText(m_hwnd, IDC_QUIET, std::to_wstring(r.quietBeforeMs));
    SetText(m_hwnd, IDC_MINLOUD, std::to_wstring(r.minLoudMs));
    SetText(m_hwnd, IDC_COOLDOWN, std::to_wstring(r.cooldownMs));
    SetText(m_hwnd, IDC_SUPPRESS, std::to_wstring(r.suppressAfterInputMs));
    SendMessageW(GetDlgItem(m_hwnd, IDC_SCOPE), CB_SETCURSEL, static_cast<WPARAM>(r.suppressScope), 0);
    Button_SetCheck(GetDlgItem(m_hwnd, IDC_BORDER), r.showBorder ? BST_CHECKED : BST_UNCHECKED);
    SetText(m_hwnd, IDC_COLOR, ColorHex(r.borderColor));
    SetText(m_hwnd, IDC_THICKNESS, std::to_wstring(r.borderThickness));
    SetText(m_hwnd, IDC_DURATION, std::to_wstring(r.flashDurationMs));
    Button_SetCheck(GetDlgItem(m_hwnd, IDC_BLINK), r.blink ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(m_hwnd, IDC_LABEL), r.showLabel ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(m_hwnd, IDC_TASKBAR), r.flashTaskbar ? BST_CHECKED : BST_UNCHECKED);
}

bool AlertsDialog::ReadRuleFromControls(AlertRule& r, bool showErrors) {
    auto fail = [&](const wchar_t* message, int id) {
        if (showErrors) {
            MessageBoxW(m_hwnd, message, L"Window Alerts", MB_ICONWARNING);
            SetFocus(GetDlgItem(m_hwnd, id));
        }
        return false;
    };
    r.name = GetText(m_hwnd, IDC_NAME);
    if (r.name.empty()) r.name = L"Alert";
    r.enabled = Button_GetCheck(GetDlgItem(m_hwnd, IDC_ENABLED)) == BST_CHECKED;
    r.processPattern = GetText(m_hwnd, IDC_PATTERN);
    if (r.processPattern.empty()) return fail(L"Enter the process name to watch, e.g. Wow.exe (wildcards * and ? work; separate several with ;).", IDC_PATTERN);
    if (!ParseFloat(GetText(m_hwnd, IDC_THRESHOLD), 0.1f, 100.0f, r.thresholdPercent)) return fail(L"Trigger level must be a number from 0.1 to 100.", IDC_THRESHOLD);
    if (!ParseInt(GetText(m_hwnd, IDC_QUIET), 0, 60000, r.quietBeforeMs)) return fail(L"Quiet before must be 0 - 60000 ms.", IDC_QUIET);
    if (!ParseInt(GetText(m_hwnd, IDC_MINLOUD), 0, 10000, r.minLoudMs)) return fail(L"Must stay loud must be 0 - 10000 ms.", IDC_MINLOUD);
    if (!ParseInt(GetText(m_hwnd, IDC_COOLDOWN), 0, 600000, r.cooldownMs)) return fail(L"Cooldown must be 0 - 600000 ms.", IDC_COOLDOWN);
    if (!ParseInt(GetText(m_hwnd, IDC_SUPPRESS), 0, 60000, r.suppressAfterInputMs)) return fail(L"Ignore after input must be 0 - 60000 ms.", IDC_SUPPRESS);
    int scope = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_SCOPE), CB_GETCURSEL, 0, 0));
    r.suppressScope = scope == 1 ? 1 : 0;
    r.showBorder = Button_GetCheck(GetDlgItem(m_hwnd, IDC_BORDER)) == BST_CHECKED;
    if (!ParseColor(GetText(m_hwnd, IDC_COLOR), r.borderColor)) return fail(L"Colour must be 6 hex digits, e.g. FF2828.", IDC_COLOR);
    if (!ParseInt(GetText(m_hwnd, IDC_THICKNESS), 2, 80, r.borderThickness)) return fail(L"Thickness must be 2 - 80 px.", IDC_THICKNESS);
    if (!ParseInt(GetText(m_hwnd, IDC_DURATION), 200, 30000, r.flashDurationMs)) return fail(L"Duration must be 200 - 30000 ms.", IDC_DURATION);
    r.blink = Button_GetCheck(GetDlgItem(m_hwnd, IDC_BLINK)) == BST_CHECKED;
    r.showLabel = Button_GetCheck(GetDlgItem(m_hwnd, IDC_LABEL)) == BST_CHECKED;
    r.flashTaskbar = Button_GetCheck(GetDlgItem(m_hwnd, IDC_TASKBAR)) == BST_CHECKED;
    return true;
}

bool AlertsDialog::ConfirmDiscardChanges() {
    auto& rules = m_host.GetSettings().alerts;
    if (m_selectedRule < 0 || m_selectedRule >= static_cast<int>(rules.size())) return true;
    AlertRule edited = rules[static_cast<size_t>(m_selectedRule)];
    if (ReadRuleFromControls(edited, false) && edited == rules[static_cast<size_t>(m_selectedRule)]) return true;
    std::wstring prompt = L"Save changes to rule '" + rules[static_cast<size_t>(m_selectedRule)].name + L"'?";
    int answer = MessageBoxW(m_hwnd, prompt.c_str(), L"Window Alerts", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) {
        AlertRule check;
        if (!ReadRuleFromControls(check, true)) return false;
        SaveCurrentRule();
    }
    return true;
}

void AlertsDialog::SaveCurrentRule() {
    AlertRule rule;
    if (!ReadRuleFromControls(rule, true)) return;
    auto& rules = m_host.GetSettings().alerts;
    if (m_selectedRule < 0 || m_selectedRule >= static_cast<int>(rules.size())) {
        rules.push_back(rule);
        m_selectedRule = static_cast<int>(rules.size()) - 1;
    } else {
        rules[static_cast<size_t>(m_selectedRule)] = rule;
    }
    m_host.OnRulesChanged();
    int keep = m_selectedRule;
    RefreshRuleList();
    SelectRule(keep);
}

LRESULT CALLBACK AlertsDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AlertsDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<AlertsDialog*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<AlertsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->m_hwnd = nullptr;
        self->m_liveList = nullptr;
        self->m_ruleList = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (self) return self->Handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AlertsDialog::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kLiveTimer) RefreshLive();
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom == IDC_LIVE && hdr->code == LVN_GETDISPINFOW) {
                OnLiveDispInfo(reinterpret_cast<NMLVDISPINFOW*>(lParam));
                return 0;
            }
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            auto& rules = m_host.GetSettings().alerts;
            switch (id) {
                case IDC_RULES:
                    if (code == LBN_SELCHANGE) {
                        int index = static_cast<int>(SendMessageW(m_ruleList, LB_GETCURSEL, 0, 0));
                        if (index != m_selectedRule) {
                            if (ConfirmDiscardChanges()) {
                                SelectRule(index);
                            } else {
                                SendMessageW(m_ruleList, LB_SETCURSEL, static_cast<WPARAM>(m_selectedRule), 0);
                            }
                        }
                    }
                    return 0;
                case IDC_ADD:
                case IDC_PRESET: {
                    if (!ConfirmDiscardChanges()) return 0;
                    AlertRule rule = id == IDC_PRESET ? MakeWowFishingPreset() : AlertRule();
                    if (id == IDC_ADD) rule.processPattern = L"";
                    rules.push_back(rule);
                    if (id == IDC_PRESET) m_host.OnRulesChanged();
                    RefreshRuleList();
                    SelectRule(static_cast<int>(rules.size()) - 1);
                    if (id == IDC_ADD) SetFocus(GetDlgItem(m_hwnd, IDC_PATTERN));
                    return 0;
                }
                case IDC_REMOVE:
                    if (m_selectedRule >= 0 && m_selectedRule < static_cast<int>(rules.size())) {
                        std::wstring prompt = L"Remove rule '" + rules[static_cast<size_t>(m_selectedRule)].name + L"'?";
                        if (MessageBoxW(m_hwnd, prompt.c_str(), L"Window Alerts", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            rules.erase(rules.begin() + m_selectedRule);
                            m_host.OnRulesChanged();
                            RefreshRuleList();
                            SelectRule((std::min)(m_selectedRule, static_cast<int>(rules.size()) - 1));
                        }
                    }
                    return 0;
                case IDC_SAVE:
                    SaveCurrentRule();
                    return 0;
                case IDC_PICK: {
                    static COLORREF custom[16] = {};
                    CHOOSECOLORW cc{};
                    cc.lStructSize = sizeof(cc);
                    cc.hwndOwner = m_hwnd;
                    cc.lpCustColors = custom;
                    ParseColor(GetText(m_hwnd, IDC_COLOR), cc.rgbResult);
                    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
                    if (ChooseColorW(&cc)) SetText(m_hwnd, IDC_COLOR, ColorHex(cc.rgbResult));
                    return 0;
                }
                case IDC_TEST:
                case IDC_FROM_SELECTED: {
                    int sel = SelectedLiveIndex();
                    if (sel < 0 || sel >= static_cast<int>(m_live.size())) {
                        MessageBoxW(m_hwnd, L"Select a session in the live list first.", L"Window Alerts", MB_ICONINFORMATION);
                        return 0;
                    }
                    const LiveSessionInfo s = m_live[static_cast<size_t>(sel)];
                    if (id == IDC_TEST) {
                        AlertRule style;
                        if (!ReadRuleFromControls(style, false)) style = AlertRule();
                        style.showBorder = true;
                        m_host.TestFlash(s.processId, s.processName, style, L"Test: " + s.processName + L" (PID " + std::to_wstring(s.processId) + L")");
                    } else {
                        if (!ConfirmDiscardChanges()) return 0;
                        AlertRule rule;
                        rule.name = s.processName;
                        rule.processPattern = s.processName;
                        if (s.peakRecentMax > 0.02f) rule.thresholdPercent = std::max(1.0f, std::floor(s.peakRecentMax * 70.0f));
                        rules.push_back(rule);
                        m_host.OnRulesChanged();
                        RefreshRuleList();
                        SelectRule(static_cast<int>(rules.size()) - 1);
                    }
                    return 0;
                }
                case IDC_IDENTIFY: {
                    AlertRule style;
                    style.borderColor = RGB(40, 140, 255);
                    style.flashTaskbar = false;
                    style.blink = false;
                    style.flashDurationMs = 4000;
                    std::vector<DWORD> done;
                    for (const auto& s : m_live) {
                        DWORD owner = s.windowOwnerPid ? s.windowOwnerPid : s.processId;
                        if (s.isSystem || !owner || std::find(done.begin(), done.end(), owner) != done.end()) continue;
                        done.push_back(owner);
                        m_host.TestFlash(s.processId, s.processName, style, s.processName + L" (PID " + std::to_wstring(s.processId) + L")");
                    }
                    if (done.empty()) MessageBoxW(m_hwnd, L"No sessions with windows yet. Start tracking and play some audio.", L"Window Alerts", MB_ICONINFORMATION);
                    return 0;
                }
            }
            break;
        }

        case WM_CLOSE:
            if (ConfirmDiscardChanges()) DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(m_hwnd, kLiveTimer);
            LOG_DEBUG("ui", L"Alerts window closed");
            return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
