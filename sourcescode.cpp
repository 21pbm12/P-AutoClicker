// ============================================================================
//  Simple Auto Clicker  -  Win32 API, single file, no external dependencies
//  Build: see BUILD.txt / README.txt
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdlib>
#include <ctime>

#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------- constants
#define ID_H_EDIT           110
#define ID_M_EDIT           111
#define ID_S_EDIT           112
#define ID_MS_EDIT          113
#define ID_OFFSET_CHECK     114
#define ID_OFFSET_EDIT      115
#define ID_BUTTON_COMBO     116
#define ID_TYPE_COMBO       117
#define ID_REPEAT_RADIO     118
#define ID_UNTIL_RADIO      119
#define ID_REPEATCOUNT_EDIT 120
#define ID_RADIUS_EDIT      102
#define ID_START_BTN        103
#define ID_CIRCLE_MODE_BTN  121
#define WM_APP_TOGGLE        (WM_APP + 1)
#define WM_APP_STATUS_ONLY   (WM_APP + 2)

static const int MIN_INTERVAL_MS = 10; // ~100 clicks/sec ceiling

enum MouseButtonSel { BTN_LEFT = 0, BTN_RIGHT = 1 };
enum ClickTypeSel    { CLICK_SINGLE = 0, CLICK_DOUBLE = 1 };
enum RepeatModeSel   { REPEAT_UNTIL_STOPPED = 0, REPEAT_N_TIMES = 1 };

static const wchar_t* MAIN_CLASS         = L"SimpleAutoClickerMain";
static const wchar_t* OVERLAY_CLASS      = L"SimpleAutoClickerOverlay";
static const wchar_t* THEME_TOGGLE_CLASS = L"SimpleAutoClickerThemeToggle";

// ------------------------------------------------------------------ globals
static HWND g_hMain = nullptr, g_hOverlay = nullptr, g_hThemeToggle = nullptr;
static HWND g_hHours, g_hMins, g_hSecs, g_hMs;
static HWND g_hOffsetCheck, g_hOffsetEdit;
static HWND g_hButtonCombo, g_hTypeCombo;
static HWND g_hRepeatRadio, g_hUntilRadio, g_hRepeatCountEdit;
static HWND g_hRadius, g_hStartBtn, g_hStatus, g_hNote;

static HFONT g_fontBold = nullptr, g_fontNormal = nullptr, g_fontSmall = nullptr;

static bool g_darkMode = false;
static HBRUSH g_bgBrushLight = nullptr, g_bgBrushDark = nullptr;
static HBRUSH g_editBrushLight = nullptr, g_editBrushDark = nullptr;

static POINT g_center = { 0, 0 };
static int   g_radius = 60;

static int  g_baseIntervalMs = 100;
static bool g_offsetEnabled  = false;
static int  g_offsetMs       = 40;
static int  g_repeatCount    = 1;

static MouseButtonSel g_mouseButton = BTN_LEFT;
static ClickTypeSel   g_clickType   = CLICK_SINGLE;
static RepeatModeSel  g_repeatMode  = REPEAT_UNTIL_STOPPED;
static bool g_circleModeOn = true; // ON = random point inside circle (existing behaviour). OFF = fixed single point (circle centre).
static HWND g_hCircleModeBtn = nullptr;

static volatile bool g_running = false;
static volatile bool g_quit    = false;
static int  g_clicksDone = 0;
static HANDLE g_clickThread = nullptr;

static HHOOK g_kbHook = nullptr;
static bool  g_ctrlDown = false, g_altDown = false, g_latched = false;

static WNDPROC g_origEditProc = nullptr;

// ---------------------------------------------------------------- utilities
static int GetEditInt(HWND h, int fallback) {
    wchar_t buf[32];
    GetWindowTextW(h, buf, 32);
    if (buf[0] == 0) return fallback;
    return _wtoi(buf);
}
static void SetEditInt(HWND h, int v) {
    wchar_t buf[32];
    wsprintfW(buf, L"%d", v);
    SetWindowTextW(h, buf);
}

// -------------------------------------------------------- overlay geometry
static void PositionOverlay() {
    if (!g_hOverlay) return;
    int size = g_radius * 2 + 8;
    int x = g_center.x - size / 2;
    int y = g_center.y - size / 2;
    MoveWindow(g_hOverlay, x, y, size, size, TRUE);

    HRGN outer = CreateEllipticRgn(0, 0, size, size);
    HRGN inner = CreateEllipticRgn(4, 4, size - 4, size - 4);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    SetWindowRgn(g_hOverlay, outer, TRUE); // ownership transferred
    DeleteObject(inner);
    InvalidateRect(g_hOverlay, nullptr, TRUE);
}

// ------------------------------------------------------------- UI -> state
static int FindCfgIndex(HWND h); // fwd decl (drag-edit table, defined below)

static void UpdateAllFromUI() {
    int h  = GetEditInt(g_hHours, 0);  if (h  < 0) h  = 0;
    int m  = GetEditInt(g_hMins, 0);   if (m  < 0) m  = 0;
    int s  = GetEditInt(g_hSecs, 0);   if (s  < 0) s  = 0;
    int ms = GetEditInt(g_hMs, 100);   if (ms < 0) ms = 0;
    g_baseIntervalMs = ((h * 3600 + m * 60 + s) * 1000) + ms;

    g_offsetMs = GetEditInt(g_hOffsetEdit, 0);
    if (g_offsetMs < 0) g_offsetMs = 0;

    int rad = GetEditInt(g_hRadius, g_radius);
    if (rad < 1) rad = 1;
    if (rad != g_radius) { g_radius = rad; PositionOverlay(); }

    int rc = GetEditInt(g_hRepeatCountEdit, 1);
    if (rc < 1) rc = 1;
    g_repeatCount = rc;

    wchar_t buf[160];
    if (g_baseIntervalMs < MIN_INTERVAL_MS) {
        wsprintfW(buf, L"Fastest reliable speed is %d ms (~%d clicks/sec). Lower values are capped to this.",
                  MIN_INTERVAL_MS, 1000 / MIN_INTERVAL_MS);
    } else {
        int cps = (g_baseIntervalMs > 0) ? (1000 / g_baseIntervalMs) : 1000;
        wsprintfW(buf, L"Current interval: %d ms (~%d clicks/sec)", g_baseIntervalMs, cps);
    }
    SetWindowTextW(g_hNote, buf);
}

static void UpdateStatusLabel() {
    if (g_running) {
        SetWindowTextW(g_hStatus, L"Status: ON (clicking)");
        SetWindowTextW(g_hStartBtn, L"Stop  (Ctrl+Alt)");
    } else {
        SetWindowTextW(g_hStatus, L"Status: OFF");
        SetWindowTextW(g_hStartBtn, L"Start  (Ctrl+Alt)");
    }
    InvalidateRect(g_hStatus, nullptr, TRUE);
}

static void UpdateCircleModeButtonLabel() {
    SetWindowTextW(g_hCircleModeBtn, g_circleModeOn ? L"Circle Mode: ON" : L"Circle Mode: OFF");
}

static void ToggleRunning() {
    g_running = !g_running;
    if (g_running) g_clicksDone = 0;
    UpdateStatusLabel();
}

// ------------------------------------------------------------- theme
static void ApplyThemeRepaint() {
    InvalidateRect(g_hMain, nullptr, TRUE);
    HWND child = GetWindow(g_hMain, GW_CHILD);
    while (child) { InvalidateRect(child, nullptr, TRUE); child = GetWindow(child, GW_HWNDNEXT); }
}
static void ToggleTheme() {
    g_darkMode = !g_darkMode;
    ApplyThemeRepaint();
}

// -------------------------------------------------------------- click loop
static DWORD WINAPI ClickThreadProc(LPVOID) {
    while (!g_quit) {
        if (g_running) {
            int x, y;
            if (g_circleModeOn) {
                double angle = ((double)rand() / RAND_MAX) * 6.28318530718;
                double r     = g_radius * sqrt((double)rand() / RAND_MAX);
                x = g_center.x + (int)(cos(angle) * r);
                y = g_center.y + (int)(sin(angle) * r);
            } else {
                x = g_center.x;
                y = g_center.y;
            }
            SetCursorPos(x, y);

            DWORD downFlag = (g_mouseButton == BTN_RIGHT) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
            DWORD upFlag   = (g_mouseButton == BTN_RIGHT) ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;

            INPUT in[4] = {};
            in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = downFlag;
            in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = upFlag;
            int count = 2;
            if (g_clickType == CLICK_DOUBLE) {
                in[2] = in[0]; in[3] = in[1];
                count = 4;
            }
            SendInput(count, in, sizeof(INPUT));

            g_clicksDone++;
            if (g_repeatMode == REPEAT_N_TIMES && g_clicksDone >= g_repeatCount) {
                g_running = false;
                PostMessage(g_hMain, WM_APP_STATUS_ONLY, 0, 0);
            }

            int wait = g_baseIntervalMs;
            if (g_offsetEnabled && g_offsetMs > 0) {
                int off = (rand() % (2 * g_offsetMs + 1)) - g_offsetMs;
                wait += off;
            }
            if (wait < MIN_INTERVAL_MS) wait = MIN_INTERVAL_MS;
            Sleep(wait);
        } else {
            Sleep(15);
        }
    }
    return 0;
}

// ------------------------------------------------- low level keyboard hook
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        if (k->vkCode == VK_LCONTROL || k->vkCode == VK_RCONTROL) {
            if (isDown) g_ctrlDown = true;
            if (isUp)   g_ctrlDown = false;
        } else if (k->vkCode == VK_LMENU || k->vkCode == VK_RMENU) {
            if (isDown) g_altDown = true;
            if (isUp)   g_altDown = false;
        }
        if (g_ctrlDown && g_altDown && !g_latched) {
            g_latched = true;
            PostMessage(g_hMain, WM_APP_TOGGLE, 0, 0);
        } else if (!(g_ctrlDown && g_altDown)) {
            g_latched = false;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// ------------------------------------------------- overlay window procedure
static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        HBRUSH br = CreateSolidBrush(RGB(255, 60, 60));
        HPEN   pen = CreatePen(PS_SOLID, 3, RGB(255, 30, 30));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBr  = SelectObject(hdc, br);
        Ellipse(hdc, 2, 2, rc.right - 2, rc.bottom - 2);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(pen);
        DeleteObject(br);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_MOVE: {
        RECT rc; GetWindowRect(hWnd, &rc);
        g_center.x = (rc.left + rc.right) / 2;
        g_center.y = (rc.top + rc.bottom) / 2;
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// --------------------------------------------------- theme-toggle button
static LRESULT CALLBACK ThemeToggleProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        // circle colour is the OPPOSITE of the current theme
        COLORREF fill = g_darkMode ? RGB(245, 245, 245) : RGB(20, 20, 20);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(130, 130, 130));
        HGDIOBJ op = SelectObject(hdc, pen), ob = SelectObject(hdc, br);
        Ellipse(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, op); SelectObject(hdc, ob);
        DeleteObject(pen); DeleteObject(br);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        ToggleTheme();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// --------------------------------------- drag-adjustable numeric edit box
struct DragState { bool captured = false; bool dragging = false; int startY = 0; int startVal = 0; };
struct EditCfg    { HWND hwnd; int minVal; int step; };
static EditCfg    g_editCfgs[10];
static DragState  g_dragStates[10];
static int        g_editCfgCount = 0;

static int FindCfgIndex(HWND h) {
    for (int i = 0; i < g_editCfgCount; i++) if (g_editCfgs[i].hwnd == h) return i;
    return -1;
}

static LRESULT CALLBACK DragEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int idx = FindCfgIndex(hWnd);
    if (idx < 0) return CallWindowProc(g_origEditProc, hWnd, msg, wParam, lParam);
    EditCfg& cfg = g_editCfgs[idx];
    DragState& st = g_dragStates[idx];

    switch (msg) {
    case WM_LBUTTONDOWN:
        st.captured = true; st.dragging = false;
        st.startY = GET_Y_LPARAM(lParam);
        st.startVal = GetEditInt(hWnd, cfg.minVal);
        SetCapture(hWnd);
        break;
    case WM_MOUSEMOVE:
        if (st.captured) {
            int dy = st.startY - GET_Y_LPARAM(lParam);
            if (!st.dragging && abs(dy) > 3) st.dragging = true;
            if (st.dragging) {
                int newVal = st.startVal + dy * cfg.step / 3;
                if (newVal < cfg.minVal) newVal = cfg.minVal;
                SetEditInt(hWnd, newVal);
                UpdateAllFromUI();
                return 0;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (st.captured) {
            ReleaseCapture(); st.captured = false;
            if (st.dragging) { st.dragging = false; return 0; }
        }
        break;
    case WM_KILLFOCUS:
        UpdateAllFromUI();
        break;
    }
    return CallWindowProc(g_origEditProc, hWnd, msg, wParam, lParam);
}

static void RegisterDragEdit(HWND h, int minVal, int step) {
    g_editCfgs[g_editCfgCount] = { h, minVal, step };
    g_dragStates[g_editCfgCount] = DragState();
    g_editCfgCount++;
    if (!g_origEditProc) g_origEditProc = (WNDPROC)GetWindowLongPtr(h, GWLP_WNDPROC);
    SetWindowLongPtr(h, GWLP_WNDPROC, (LONG_PTR)DragEditProc);
}

// ---------------------------------------------------- main window creation
static HWND MakeStatic(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
    HWND s = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
    SendMessage(s, WM_SETFONT, (WPARAM)font, TRUE);
    return s;
}
static HWND MakeEdit(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id, HFONT font) {
    HWND e = CreateWindowW(L"EDIT", text, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessage(e, WM_SETFONT, (WPARAM)font, TRUE);
    return e;
}

static void CreateControls(HWND hWnd) {
    g_fontNormal = CreateFontW(16, 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontBold = CreateFontW(18, 0,0,0, FW_BOLD, FALSE,FALSE,FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(14, 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    RECT rcClient; GetClientRect(hWnd, &rcClient);
    g_hThemeToggle = CreateWindowExW(0, THEME_TOGGLE_CLASS, L"", WS_CHILD | WS_VISIBLE,
        rcClient.right - 38, 10, 24, 24, hWnd, nullptr, nullptr, nullptr);

    MakeStatic(hWnd, L"HOW TO USE  (read this first)", 15, 10, 380, 24, g_fontBold);
    const wchar_t* help =
        L"1) Drag the RED circle to reposition it (it starts at screen centre).\n"
        L"2) Set interval, offset, button/click-type and repeat below.\n"
        L"3) Press CTRL + ALT anywhere to toggle ON/OFF, or use the button.\n"
        L"4) The small circle, top-right, switches between light/dark theme.\n"
        L"5) Circle Mode ON = clicks land at random points in the circle;\n"
        L"    OFF = every click lands on the exact same fixed point.";
    MakeStatic(hWnd, help, 15, 34, 450, 112, g_fontNormal);

    // ---- group: Click interval
    CreateWindowW(L"BUTTON", L"Click interval", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        15, 152, 450, 105, hWnd, nullptr, nullptr, nullptr);
    g_hHours = MakeEdit(hWnd, L"0", 25, 175, 42, 24, ID_H_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"hours", 70, 178, 45, 20, g_fontNormal);
    g_hMins  = MakeEdit(hWnd, L"0", 118, 175, 42, 24, ID_M_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"mins", 163, 178, 40, 20, g_fontNormal);
    g_hSecs  = MakeEdit(hWnd, L"0", 206, 175, 42, 24, ID_S_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"secs", 251, 178, 40, 20, g_fontNormal);
    g_hMs    = MakeEdit(hWnd, L"100", 294, 175, 55, 24, ID_MS_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"milliseconds", 352, 178, 105, 20, g_fontNormal);

    g_hOffsetCheck = CreateWindowW(L"BUTTON", L"Random offset \u00B1",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 207, 160, 22, hWnd, (HMENU)ID_OFFSET_CHECK, nullptr, nullptr);
    SendMessage(g_hOffsetCheck, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    g_hOffsetEdit = MakeEdit(hWnd, L"40", 190, 205, 55, 24, ID_OFFSET_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"milliseconds", 252, 208, 105, 20, g_fontNormal);

    g_hNote = MakeStatic(hWnd, L"", 25, 235, 430, 18, g_fontSmall);

    // ---- group: Click options
    CreateWindowW(L"BUTTON", L"Click options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        15, 270, 215, 95, hWnd, nullptr, nullptr, nullptr);
    MakeStatic(hWnd, L"Mouse button:", 25, 295, 95, 20, g_fontNormal);
    g_hButtonCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        125, 292, 95, 100, hWnd, (HMENU)ID_BUTTON_COMBO, nullptr, nullptr);
    SendMessage(g_hButtonCombo, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessageW(g_hButtonCombo, CB_ADDSTRING, 0, (LPARAM)L"Left");
    SendMessageW(g_hButtonCombo, CB_ADDSTRING, 0, (LPARAM)L"Right");
    SendMessage(g_hButtonCombo, CB_SETCURSEL, 0, 0);

    MakeStatic(hWnd, L"Click type:", 25, 327, 95, 20, g_fontNormal);
    g_hTypeCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        125, 324, 95, 100, hWnd, (HMENU)ID_TYPE_COMBO, nullptr, nullptr);
    SendMessage(g_hTypeCombo, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessageW(g_hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Single");
    SendMessageW(g_hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Double");
    SendMessage(g_hTypeCombo, CB_SETCURSEL, 0, 0);

    // ---- group: Click repeat
    CreateWindowW(L"BUTTON", L"Click repeat", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        240, 270, 225, 95, hWnd, nullptr, nullptr, nullptr);
    g_hRepeatRadio = CreateWindowW(L"BUTTON", L"Repeat", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        250, 293, 70, 20, hWnd, (HMENU)ID_REPEAT_RADIO, nullptr, nullptr);
    SendMessage(g_hRepeatRadio, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    g_hRepeatCountEdit = MakeEdit(hWnd, L"1", 325, 291, 50, 24, ID_REPEATCOUNT_EDIT, g_fontNormal);
    MakeStatic(hWnd, L"times", 380, 294, 45, 20, g_fontNormal);

    g_hUntilRadio = CreateWindowW(L"BUTTON", L"Repeat until stopped", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        250, 323, 200, 20, hWnd, (HMENU)ID_UNTIL_RADIO, nullptr, nullptr);
    SendMessage(g_hUntilRadio, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessage(g_hUntilRadio, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(g_hRepeatCountEdit, FALSE);

    // ---- circle radius + circle mode + start/stop + status
    MakeStatic(hWnd, L"Circle radius (px):", 15, 379, 180, 20, g_fontNormal);
    g_hRadius = MakeEdit(hWnd, L"60", 220, 377, 80, 24, ID_RADIUS_EDIT, g_fontNormal);

    g_hCircleModeBtn = CreateWindowW(L"BUTTON", L"Circle Mode: ON", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        310, 376, 155, 26, hWnd, (HMENU)ID_CIRCLE_MODE_BTN, nullptr, nullptr);
    SendMessage(g_hCircleModeBtn, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    g_hStartBtn = CreateWindowW(L"BUTTON", L"Start  (Ctrl+Alt)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        15, 413, 450, 36, hWnd, (HMENU)ID_START_BTN, nullptr, nullptr);
    SendMessage(g_hStartBtn, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    g_hStatus = MakeStatic(hWnd, L"Status: OFF", 15, 457, 450, 24, g_fontBold);

    RegisterDragEdit(g_hHours, 0, 1);
    RegisterDragEdit(g_hMins, 0, 1);
    RegisterDragEdit(g_hSecs, 0, 1);
    RegisterDragEdit(g_hMs, 0, 5);
    RegisterDragEdit(g_hOffsetEdit, 0, 2);
    RegisterDragEdit(g_hRadius, 1, 2);
    RegisterDragEdit(g_hRepeatCountEdit, 1, 1);

    UpdateAllFromUI();
}

// ------------------------------------------------------- main window procedure
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hWnd);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        HWND ctl = (HWND)lParam;
        if (id == ID_START_BTN && code == BN_CLICKED) {
            ToggleRunning();
        } else if (id == ID_OFFSET_CHECK && code == BN_CLICKED) {
            g_offsetEnabled = (SendMessage(g_hOffsetCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(g_hOffsetEdit, g_offsetEnabled);
            UpdateAllFromUI();
        } else if (id == ID_REPEAT_RADIO && code == BN_CLICKED) {
            g_repeatMode = REPEAT_N_TIMES;
            EnableWindow(g_hRepeatCountEdit, TRUE);
        } else if (id == ID_UNTIL_RADIO && code == BN_CLICKED) {
            g_repeatMode = REPEAT_UNTIL_STOPPED;
            EnableWindow(g_hRepeatCountEdit, FALSE);
        } else if (id == ID_BUTTON_COMBO && code == CBN_SELCHANGE) {
            g_mouseButton = (MouseButtonSel)SendMessage(g_hButtonCombo, CB_GETCURSEL, 0, 0);
        } else if (id == ID_TYPE_COMBO && code == CBN_SELCHANGE) {
            g_clickType = (ClickTypeSel)SendMessage(g_hTypeCombo, CB_GETCURSEL, 0, 0);
        } else if (id == ID_CIRCLE_MODE_BTN && code == BN_CLICKED) {
            g_circleModeOn = !g_circleModeOn;
            UpdateCircleModeButtonLabel();
        } else if (code == EN_CHANGE && ctl != nullptr && FindCfgIndex(ctl) >= 0) {
           
            UpdateAllFromUI();
        }
        return 0;
    }

    case WM_APP_TOGGLE:
        ToggleRunning();
        return 0;
    case WM_APP_STATUS_ONLY: // repeat-count finished on its own
        UpdateStatusLabel();
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_darkMode ? g_bgBrushDark : g_bgBrushLight);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        if (hCtl == g_hStatus) SetTextColor(hdcStatic, g_running ? RGB(0,180,0) : RGB(210,40,40));
        else SetTextColor(hdcStatic, g_darkMode ? RGB(235,235,235) : RGB(0,0,0));
        return (LRESULT)(g_darkMode ? g_bgBrushDark : g_bgBrushLight);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_darkMode ? RGB(235,235,235) : RGB(0,0,0));
        SetBkColor(hdc, g_darkMode ? RGB(55,55,55) : RGB(255,255,255));
        return (LRESULT)(g_darkMode ? g_editBrushDark : g_editBrushLight);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_darkMode ? RGB(235,235,235) : RGB(0,0,0));
        SetBkColor(hdc, g_darkMode ? RGB(55,55,55) : RGB(255,255,255));
        return (LRESULT)(g_darkMode ? g_editBrushDark : g_editBrushLight);
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_darkMode ? RGB(235,235,235) : RGB(0,0,0));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)(g_darkMode ? g_bgBrushDark : g_bgBrushLight);
    }

    case WM_DESTROY:
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------- WinMain
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    srand((unsigned)time(nullptr));
    timeBeginPeriod(1);

    g_bgBrushLight   = CreateSolidBrush(RGB(255,255,255));
    g_bgBrushDark    = CreateSolidBrush(RGB(30,30,30));
    g_editBrushLight = CreateSolidBrush(RGB(255,255,255));
    g_editBrushDark  = CreateSolidBrush(RGB(55,55,55));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWndProc; wc.hInstance = hInst; wc.lpszClassName = MAIN_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(1));   
    RegisterClassW(&wc);

    WNDCLASSW wco = {};
    wco.lpfnWndProc = OverlayProc; wco.hInstance = hInst; wco.lpszClassName = OVERLAY_CLASS;
    wco.hCursor = LoadCursor(nullptr, IDC_SIZEALL); wco.hbrBackground = nullptr;
    RegisterClassW(&wco);

    WNDCLASSW wct = {};
    wct.lpfnWndProc = ThemeToggleProc; wct.hInstance = hInst; wct.lpszClassName = THEME_TOGGLE_CLASS;
    wct.hCursor = LoadCursor(nullptr, IDC_HAND); wct.hbrBackground = nullptr;
    RegisterClassW(&wct);

    g_hMain = CreateWindowW(MAIN_CLASS, L"P Auto Clicker",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 570,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hMain, nCmdShow);
    UpdateWindow(g_hMain);

    g_center.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    g_center.y = GetSystemMetrics(SM_CYSCREEN) / 2;

    g_hOverlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(g_hOverlay, 0, 200, LWA_ALPHA);
    PositionOverlay();
    ShowWindow(g_hOverlay, SW_SHOW);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    g_clickThread = CreateThread(nullptr, 0, ClickThreadProc, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_quit = true;
    if (g_clickThread) { WaitForSingleObject(g_clickThread, 500); CloseHandle(g_clickThread); }
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    DeleteObject(g_bgBrushLight); DeleteObject(g_bgBrushDark);
    DeleteObject(g_editBrushLight); DeleteObject(g_editBrushDark);
    timeEndPeriod(1);
    return 0;
}
