/* The taskbar widget: placement, layered window, tooltip, menu, worker thread. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "render.h"

#define WM_APP_DATA (WM_APP + 1)
#define TIMER_PLACE 1
#define PLACE_MS 2000

#define ID_REFRESH 100
#define ID_INT_BASE 101 /* 101..104 = 5, 10, 15, 30 min */
#define ID_REMAIN 110
#define ID_AUTORUN 111
#define ID_EXIT 112

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"ClaudeTaskbarUsage"
#define CLS_CTL L"ClaudeTaskbarUsage.Controller"
#define CLS_WIDGET L"ClaudeTaskbarUsage.Widget"

static const int k_intervals[4] = { 5, 10, 15, 30 };

static struct {
    HINSTANCE inst;
    app_config *cfg;
    HWND ctl;             /* hidden top-level window: messages, menu owner */
    HWND wnd;             /* the widget */
    HWND tip;             /* tooltip */
    HWND parent;          /* Shell_TrayWnd in child mode */
    int child_mode;       /* 1 = child of Shell_TrayWnd, 0 = topmost popup */
    UINT msg_taskbar_created;
    CRITICAL_SECTION lock;
    ctu_view view;        /* copy for the UI thread, under lock */
    ctu_engine eng;       /* owned by the worker thread */
    HANDLE thread, ev_stop, ev_refresh, ev_wake;
    volatile LONG interval_min;
    render_in last_in;
    render_out bmp;
    int have_bmp;
    int pushed;           /* the bitmap in bmp was given to the window */
    RECT last_rect;       /* last screen rectangle */
    int shown;
    int theme_override;   /* -1 none, 0 dark, 1 light (CTU_THEME) */
    int offset_x;         /* test aid CTU_OFFSET_X, pixels */
    HWINEVENTHOOK hook;
} g;

/* ---- worker thread ------------------------------------------------------------------------ */

static void publish(void)
{
    EnterCriticalSection(&g.lock);
    g.view = g.eng.v;
    LeaveCriticalSection(&g.lock);
    PostMessageW(g.ctl, WM_APP_DATA, 0, 0);
}

static DWORD WINAPI worker(LPVOID param)
{
    (void)param;
    int force = 0, woken = 1, first = 1;
    for (;;) {
        g.eng.v.interval_min = (int)g.interval_min;
        int before = g.eng.v.requests_made;
        int changed = engine_step(&g.eng, ctu_now_ms(), force);
        if (changed || force || woken) publish();
        /* Start-up and a request (the TLS stack) touch many pages that are not needed while idle. */
        if (first || g.eng.v.requests_made != before) {
            if (first) Sleep(1500); /* let the UI thread draw first */
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            first = 0;
        }
        long long wait = engine_next_due(&g.eng) - ctu_now_ms();
        if (wait > 60000) wait = 60000; /* Claude Code's cache is checked at least every 60 s */
        if (g.eng.v.note_kind == NOTE_LOGIN && wait > 5000)
            wait = 5000; /* a renewed login (credentials file changed) is seen within about 5 s */
        if (wait < 1000) wait = 1000;
        HANDLE hs[3] = { g.ev_stop, g.ev_refresh, g.ev_wake };
        DWORD r = WaitForMultipleObjects(3, hs, FALSE, (DWORD)wait);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        force = (r == WAIT_OBJECT_0 + 1);
        woken = (r == WAIT_OBJECT_0 + 2);
    }
    return 0;
}

static void snapshot(ctu_view *v)
{
    EnterCriticalSection(&g.lock);
    *v = g.view;
    LeaveCriticalSection(&g.lock);
}

/* ---- helpers ------------------------------------------------------------------------------ */

static int is_light_theme(void)
{
    if (g.theme_override >= 0) return g.theme_override;
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return 0;
}

static int dpi_of(HWND h)
{
    typedef UINT(WINAPI * fn_t)(HWND);
    static fn_t fn = NULL;
    static int loaded = 0;
    if (!loaded) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        fn = u ? (fn_t)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
        loaded = 1;
    }
    if (fn && h) {
        UINT d = fn(h);
        if (d) return (int)d;
    }
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

static int autorun_get(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0;
    LONG r = RegQueryValueExW(k, RUN_VALUE, NULL, &type, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void autorun_set(int on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[CTU_PATH_MAX], val[CTU_PATH_MAX + 4];
        DWORD n = GetModuleFileNameW(NULL, path, CTU_PATH_MAX);
        if (n > 0 && n < CTU_PATH_MAX) {
            _snwprintf(val, CTU_PATH_MAX + 4, L"\"%ls\"", path);
            val[CTU_PATH_MAX + 3] = 0;
            RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

/* A full-screen window in the foreground on the primary monitor (popup mode only). */
static int fullscreen_foreground(HWND tray)
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == tray || fg == g.wnd || fg == g.ctl) return 0;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return 0;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi) || !(mi.dwFlags & MONITORINFOF_PRIMARY)) return 0;
    RECT r;
    if (!GetWindowRect(fg, &r)) return 0;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

/* ---- the widget window -------------------------------------------------------------------- */

static void destroy_widget(void)
{
    if (g.tip) {
        DestroyWindow(g.tip);
        g.tip = NULL;
    }
    if (g.wnd && IsWindow(g.wnd)) DestroyWindow(g.wnd);
    g.wnd = NULL;
    g.parent = NULL;
    g.pushed = 0;
    g.shown = 0;
    SetRectEmpty(&g.last_rect);
}

static void create_tooltip(void)
{
    g.tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, g.ctl, NULL, g.inst, NULL);
    if (!g.tip) return;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g.wnd;
    ti.uId = 1;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    GetClientRect(g.wnd, &ti.rect);
    SendMessageW(g.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g.tip, TTM_SETMAXTIPWIDTH, 0, 900);
    SendMessageW(g.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
}

static void update_tool_rect(void)
{
    if (!g.tip || !g.wnd) return;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.hwnd = g.wnd;
    ti.uId = 1;
    GetClientRect(g.wnd, &ti.rect);
    SendMessageW(g.tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

static int create_widget(HWND tray)
{
    if (g.tip) {
        /* The old widget went away with Explorer; its tooltip is still here. */
        DestroyWindow(g.tip);
        g.tip = NULL;
    }
    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!g.child_mode) ex |= WS_EX_TOPMOST;
    /* Owned by the hidden controller in popup mode: no taskbar button, no Alt-Tab entry. */
    g.wnd = CreateWindowExW(ex, CLS_WIDGET, L"ClaudeTaskbarUsage", WS_POPUP, 0, 0, 1, 1,
                            g.child_mode ? NULL : g.ctl, NULL, g.inst, NULL);
    if (!g.wnd) return 0;
    if (g.child_mode) {
        LONG_PTR st = GetWindowLongPtrW(g.wnd, GWL_STYLE);
        SetWindowLongPtrW(g.wnd, GWL_STYLE, (st & ~(LONG_PTR)WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS);
        if (!SetParent(g.wnd, tray)) {
            /* Could not attach: fall back to the popup method. */
            DestroyWindow(g.wnd);
            g.wnd = NULL;
            g.child_mode = 0;
            return create_widget(tray);
        }
        g.parent = tray;
    }
    g.pushed = 0;
    g.shown = 0;
    SetRectEmpty(&g.last_rect);
    create_tooltip();
    return 1;
}

static void hide_widget(void)
{
    if (g.wnd && g.shown) {
        ShowWindow(g.wnd, SW_HIDE);
        g.shown = 0;
    }
}

static void build_input(render_in *in, int dpi, int height)
{
    ctu_view v;
    snapshot(&v);
    memset(in, 0, sizeof(*in));
    in->dpi = dpi;
    in->height = height;
    in->light = is_light_theme();
    ctu_bar bar;
    ctu_bar_values(v.have_data ? &v.data : NULL, g.cfg->show_remaining, &bar);
    in->n = bar.n;
    for (int i = 0; i < bar.n; i++) {
        MultiByteToWideChar(CP_UTF8, 0, bar.label[i], -1, in->label[i], 4);
        MultiByteToWideChar(CP_UTF8, 0, bar.text[i], -1, in->text[i], 8);
        in->lvl[i] = bar.level[i];
    }
    in->dim = ctu_view_dimmed(&v, ctu_now_ms());
}

static void push_bitmap(void)
{
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, g.bmp.bmp);
    SIZE sz = { g.bmp.w, g.bmp.h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g.wnd, NULL, NULL, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    g.pushed = 1;
}

/* Re-checks the taskbar, the theme and the data; renders and moves only when something changed. */
static void update(void)
{
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tray) {
        hide_widget();
        return;
    }
    if (g.wnd && (!IsWindow(g.wnd) || (g.child_mode && (g.parent != tray || GetParent(g.wnd) != tray))))
        destroy_widget();
    if (!g.wnd && !create_widget(tray)) return;

    RECT tr;
    if (!GetWindowRect(tray, &tr) || tr.right - tr.left <= tr.bottom - tr.top) {
        hide_widget(); /* no taskbar, or a vertical one */
        return;
    }
    int dpi = dpi_of(tray);
    RECT nr;
    HWND notify = FindWindowExW(tray, NULL, L"TrayNotifyWnd", NULL);
    if (!notify || !GetWindowRect(notify, &nr) || nr.left <= tr.left) {
        nr = tr;
        nr.left = tr.right - MulDiv(200, dpi, 96);
    }

    render_in in;
    build_input(&in, dpi, tr.bottom - tr.top);
    if (!g.have_bmp || memcmp(&in, &g.last_in, sizeof(in)) != 0) {
        render_out out;
        if (render_widget(&in, &out)) {
            if (g.have_bmp) DeleteObject(g.bmp.bmp);
            g.bmp = out;
            g.have_bmp = 1;
            g.last_in = in;
            g.pushed = 0;
        }
    }
    if (!g.have_bmp) return;

    if (!g.child_mode) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY), &mi);
        int autohidden = tr.top >= mi.rcMonitor.bottom - MulDiv(4, dpi, 96);
        if (autohidden || fullscreen_foreground(tray)) {
            hide_widget();
            return;
        }
    }

    int gap = MulDiv(4, dpi, 96);
    RECT want;
    want.left = nr.left - gap - g.bmp.w + g.offset_x;
    want.top = tr.top;
    want.right = want.left + g.bmp.w;
    want.bottom = want.top + g.bmp.h;

    if (!g.pushed) push_bitmap();
    if (!g.shown || !EqualRect(&want, &g.last_rect)) {
        if (g.child_mode) {
            POINT pt = { want.left, want.top };
            ScreenToClient(tray, &pt);
            SetWindowPos(g.wnd, HWND_TOP, pt.x, pt.y, g.bmp.w, g.bmp.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            SetWindowPos(g.wnd, HWND_TOPMOST, want.left, want.top, g.bmp.w, g.bmp.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        g.last_rect = want;
        g.shown = 1;
        update_tool_rect();
    } else if (g.child_mode) {
        /* Stay above the taskbar's own content. */
        if (GetWindow(tray, GW_CHILD) != g.wnd)
            SetWindowPos(g.wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowPos(g.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/* ---- menu --------------------------------------------------------------------------------- */

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    if (!m || !sub) return;
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh now");
    int sel = 0;
    for (int i = 0; i < 4; i++) {
        wchar_t t[32];
        _snwprintf(t, 32, L"%d min", k_intervals[i]);
        AppendMenuW(sub, MF_STRING, ID_INT_BASE + i, t);
        if (k_intervals[i] == g.cfg->interval_min) sel = i;
    }
    CheckMenuRadioItem(sub, ID_INT_BASE, ID_INT_BASE + 3, ID_INT_BASE + sel, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Update interval");
    AppendMenuW(m, MF_STRING | (g.cfg->show_remaining ? MF_CHECKED : 0), ID_REMAIN, L"Show remaining instead of used");
    AppendMenuW(m, MF_STRING | (autorun_get() ? MF_CHECKED : 0), ID_AUTORUN, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.ctl);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y,
                                  0, g.ctl, NULL);
    PostMessageW(g.ctl, WM_NULL, 0, 0);
    DestroyMenu(m); /* also destroys sub */
    if (cmd) PostMessageW(g.ctl, WM_COMMAND, (WPARAM)cmd, 0);
}

static void on_command(int id)
{
    if (id == ID_REFRESH) {
        SetEvent(g.ev_refresh);
    } else if (id >= ID_INT_BASE && id < ID_INT_BASE + 4) {
        g.cfg->interval_min = k_intervals[id - ID_INT_BASE];
        InterlockedExchange(&g.interval_min, g.cfg->interval_min);
        app_save_config(g.cfg);
        SetEvent(g.ev_wake);
    } else if (id == ID_REMAIN) {
        g.cfg->show_remaining = !g.cfg->show_remaining;
        app_save_config(g.cfg);
        update();
    } else if (id == ID_AUTORUN) {
        autorun_set(!autorun_get());
    } else if (id == ID_EXIT) {
        DestroyWindow(g.ctl);
    }
}

/* ---- window procedures -------------------------------------------------------------------- */

static LRESULT CALLBACK widget_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_LBUTTONDBLCLK:
        SetEvent(g.ev_refresh);
        return 0;
    case WM_RBUTTONUP:
        show_menu();
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        PostMessageW(g.ctl, WM_APP_DATA, 0, 0);
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->code == TTN_GETDISPINFOW) {
            static char tip8[4096];
            static wchar_t tipw[4096];
            ctu_view v;
            snapshot(&v);
            ctu_build_tooltip(&v, ctu_now_ms(), g.cfg->show_remaining, tip8, sizeof(tip8));
            MultiByteToWideChar(CP_UTF8, 0, tip8, -1, tipw, 4096);
            tipw[4095] = 0;
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
            di->lpszText = tipw;
            di->hinst = NULL;
            return 0;
        }
        break;
    }
    case WM_NCDESTROY:
        if (hwnd == g.wnd) {
            /* Also reached when Explorer ends and takes its child windows along. */
            g.wnd = NULL;
            g.shown = 0;
            g.pushed = 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void shutdown_all(void)
{
    KillTimer(g.ctl, TIMER_PLACE);
    if (g.hook) {
        UnhookWinEvent(g.hook);
        g.hook = NULL;
    }
    destroy_widget();
    if (g.thread) {
        SetEvent(g.ev_stop);
        /* A request in progress has 15 s timeouts; do not wait for it at exit. */
        WaitForSingleObject(g.thread, 2000);
        CloseHandle(g.thread);
        g.thread = NULL;
    }
}

static LRESULT CALLBACK ctl_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g.msg_taskbar_created && msg != 0) {
        /* Explorer restarted: attach to the new taskbar. */
        destroy_widget();
        update();
        return 0;
    }
    switch (msg) {
    case WM_APP_DATA:
    case WM_TIMER:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_THEMECHANGED:
        update();
        return 0;
    case WM_SETTINGCHANGE:
        update();
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_ENDSESSION:
        if (wp) shutdown_all();
        return 0;
    case WM_DESTROY:
        shutdown_all();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CALLBACK foreground_hook(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD tid, DWORD ms)
{
    (void)h;
    (void)ev;
    (void)hwnd;
    (void)obj;
    (void)child;
    (void)tid;
    (void)ms;
    update();
}

int widget_run(HINSTANCE inst, app_config *cfg)
{
    memset(&g, 0, sizeof(g));
    g.inst = inst;
    g.cfg = cfg;
    g.theme_override = -1;
    wchar_t env[32];
    DWORD n = GetEnvironmentVariableW(L"CTU_THEME", env, 32);
    if (n > 0 && n < 32) {
        if (!_wcsicmp(env, L"light")) g.theme_override = 1;
        if (!_wcsicmp(env, L"dark")) g.theme_override = 0;
    }
    g.child_mode = 1;
    n = GetEnvironmentVariableW(L"CTU_MODE", env, 32);
    if (n > 0 && n < 32 && !_wcsicmp(env, L"popup")) g.child_mode = 0;
    /* Test aid: move the widget by this many pixels (negative = left), for a second test instance. */
    n = GetEnvironmentVariableW(L"CTU_OFFSET_X", env, 32);
    if (n > 0 && n < 32) g.offset_x = _wtoi(env);
    /* Test aid: CTU_TEXT=gdi uses the GDI text fallback instead of DirectWrite. */
    n = GetEnvironmentVariableW(L"CTU_TEXT", env, 32);
    if (n > 0 && n < 32 && !_wcsicmp(env, L"gdi")) render_force_gdi(1);

    InitializeCriticalSection(&g.lock);
    g.interval_min = cfg->interval_min;
    app_setup_engine(cfg, &g.eng);
    g.view = g.eng.v; /* saved data shows at once */

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = ctl_proc;
    wc.lpszClassName = CLS_CTL;
    if (!RegisterClassExW(&wc)) return 1;
    wc.lpfnWndProc = widget_proc;
    wc.lpszClassName = CLS_WIDGET;
    wc.style = CS_DBLCLKS;
    if (!RegisterClassExW(&wc)) return 1;

    g.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, CLS_CTL, L"ClaudeTaskbarUsage", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst,
                            NULL);
    if (!g.ctl) return 1;
    g.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    g.ev_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g.ev_refresh = CreateEventW(NULL, FALSE, FALSE, NULL);
    g.ev_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    app_set_renew_stop(g.ev_stop); /* "Exit" ends a running login renewal at once */
    g.thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);

    SetTimer(g.ctl, TIMER_PLACE, PLACE_MS, NULL);
    if (!g.child_mode)
        g.hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, foreground_hook, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    update();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.have_bmp) DeleteObject(g.bmp.bmp);
    render_shutdown();
    return 0;
}
