/* Entry point: single instance, --once test mode, settings and paths. */
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "http.h"

static int env_w(const wchar_t *name, wchar_t *out, DWORD cap)
{
    DWORD n = GetEnvironmentVariableW(name, out, cap);
    if (n == 0 || n >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

void app_load_config(app_config *c)
{
    memset(c, 0, sizeof(*c));
    if (!env_w(L"CTU_DATA_DIR", c->data_dir, CTU_PATH_MAX)) {
        wchar_t appdata[CTU_PATH_MAX];
        if (env_w(L"APPDATA", appdata, CTU_PATH_MAX))
            _snwprintf(c->data_dir, CTU_PATH_MAX, L"%ls\\ClaudeTaskbarUsage", appdata);
    }
    c->data_dir[CTU_PATH_MAX - 1] = 0;
    if (c->data_dir[0]) CreateDirectoryW(c->data_dir, NULL);
    _snwprintf(c->ini_path, CTU_PATH_MAX, L"%ls\\settings.ini", c->data_dir);
    c->ini_path[CTU_PATH_MAX - 1] = 0;
    env_w(L"CTU_SAMPLE", c->sample_path, CTU_PATH_MAX);

    int iv = (int)GetPrivateProfileIntW(L"Settings", L"IntervalMin", 5, c->ini_path);
    c->interval_min = (iv == 5 || iv == 10 || iv == 15 || iv == 30) ? iv : 5;
    c->show_remaining = GetPrivateProfileIntW(L"Settings", L"ShowRemaining", 0, c->ini_path) ? 1 : 0;
}

void app_save_config(const app_config *c)
{
    wchar_t v[16];
    _snwprintf(v, 16, L"%d", c->interval_min);
    WritePrivateProfileStringW(L"Settings", L"IntervalMin", v, c->ini_path);
    WritePrivateProfileStringW(L"Settings", L"ShowRemaining", c->show_remaining ? L"1" : L"0", c->ini_path);
}

void app_setup_engine(const app_config *c, ctu_engine *e)
{
    engine_init(e, c->interval_min);
    wchar_t home[CTU_PATH_MAX];
    if (env_w(L"USERPROFILE", home, CTU_PATH_MAX)) {
        _snwprintf(e->cred_path, CTU_PATH_MAX, L"%ls\\.claude\\.credentials.json", home);
        _snwprintf(e->ccjson_path, CTU_PATH_MAX, L"%ls\\.claude.json", home);
        e->cred_path[CTU_PATH_MAX - 1] = 0;
        e->ccjson_path[CTU_PATH_MAX - 1] = 0;
    }
    e->fetch = ctu_http_fetch;
    e->fetch_ctx = NULL;
    if (c->data_dir[0]) {
        _snwprintf(e->last_path, CTU_PATH_MAX, L"%ls\\last.json", c->data_dir);
        e->last_path[CTU_PATH_MAX - 1] = 0;
        engine_load_saved(e);
    }
    if (c->sample_path[0]) {
        /* Test aid: the sample file replaces the network step; no token, no Claude Code cache,
         * last.json is read but never written. */
        e->fetch = ctu_sample_fetch;
        e->fetch_ctx = (void *)c->sample_path;
        e->skip_credentials = 1;
        e->use_cc_cache = 0;
        e->live_source = SRC_SAMPLE;
        e->last_path[0] = 0;
    }
    /* The VS Code usage extension's records (read only). Test aid CTU_EXT_DIR replaces the candidate list;
     * in sample mode the source is off unless CTU_EXT_DIR is set. */
    wchar_t ext_dir[CTU_PATH_MAX];
    if (env_w(L"CTU_EXT_DIR", ext_dir, CTU_PATH_MAX)) {
        wcscpy(e->ext_dirs[0], ext_dir);
        e->n_ext_dirs = 1;
    } else if (!c->sample_path[0]) {
        wchar_t appdata[CTU_PATH_MAX];
        if (env_w(L"APPDATA", appdata, CTU_PATH_MAX)) engine_default_ext_dirs(e, appdata);
    }
}

/* --once <file>: one data step, one line of result text, exit 0 or 1. */
static int run_once(const wchar_t *file)
{
    app_config cfg;
    app_load_config(&cfg);
    static ctu_engine e;
    app_setup_engine(&cfg, &e);
    long long now = ctu_now_ms();
    engine_step(&e, now, 1);
    static char line[4096];
    ctu_build_once_line(&e.v, ctu_now_ms(), line, sizeof(line));
    int rc = (e.v.have_data && e.v.note_kind == NOTE_NONE) ? 0 : 1;
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 1;
    strcat(line, "\r\n");
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wr, NULL);
    CloseHandle(h);
    return rc;
}

static void set_dpi_awareness(void)
{
    typedef BOOL(WINAPI * set_ctx_fn)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    set_ctx_fn f = u ? (set_ctx_fn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (f && f((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) return;
    typedef BOOL(WINAPI * set_aware_fn)(void);
    set_aware_fn g = u ? (set_aware_fn)(void *)GetProcAddress(u, "SetProcessDPIAware") : NULL;
    if (g) g();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)cmdline;
    (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--once") == 0) {
                if (i + 1 >= argc) {
                    LocalFree(argv);
                    return 2;
                }
                int rc = run_once(argv[i + 1]);
                LocalFree(argv);
                return rc;
            }
        }
        LocalFree(argv);
    }

    set_dpi_awareness();

    /* Test aid CTU_INSTANCE: a name suffix for the single-instance mutex, so that a test instance can run
     * next to the installed widget. Only letters, digits, '-' and '_' are used; anything else is ignored. */
    wchar_t mname[96] = L"Local\\ClaudeTaskbarUsage.SingleInstance";
    wchar_t inst_id[33];
    if (env_w(L"CTU_INSTANCE", inst_id, 33)) {
        int ok = 1;
        for (const wchar_t *p = inst_id; *p; p++)
            if (!((*p >= L'a' && *p <= L'z') || (*p >= L'A' && *p <= L'Z') || (*p >= L'0' && *p <= L'9') ||
                  *p == L'-' || *p == L'_'))
                ok = 0;
        if (ok) {
            wcscat(mname, L".");
            wcscat(mname, inst_id);
        }
    }
    HANDLE mutex = CreateMutexW(NULL, TRUE, mname);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    app_config cfg;
    app_load_config(&cfg);
    int rc = widget_run(inst, &cfg);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return rc;
}
