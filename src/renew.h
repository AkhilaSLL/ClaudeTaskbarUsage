/* Login renewal: runs "claude.exe mcp list" hidden; the Claude CLI then renews the credentials file.
 * No window code. The widget never reads, writes or logs the refresh token; nothing of the CLI's output is
 * stored (standard input, output and error are NUL). */
#ifndef CTU_RENEW_H
#define CTU_RENEW_H

#include <windows.h>
#include "engine.h"

#define CTU_RENEW_CMD_MAX 2048
#define CTU_RENEW_TIMEOUT_MS 60000

enum { RENEW_EXITED = 0, RENEW_TIMEOUT, RENEW_STOPPED, RENEW_NOT_STARTED };

typedef struct {
    wchar_t cmd[CTU_RENEW_CMD_MAX]; /* test aid CTU_RENEW_CMD (whole command line); "" = claude.exe mcp list */
    wchar_t workdir[CTU_PATH_MAX];  /* the widget's data folder; "" = the current folder */
    HANDLE stop;                    /* the widget's stop event; NULL in --once */
} ctu_renew_ctx;

/* Length of an environment block in wchar_t, with the final terminator. */
size_t ctu_env_block_len(const wchar_t *block);
/* Copies an environment block without every variable whose name starts with CLAUDE or ANTHROPIC (any case).
 * out needs ctu_env_block_len(in) + 1 wchar_t. An empty result is two NULs. Returns the wchar_t written,
 * with the terminators; 0 when out is too small. */
size_t ctu_env_filter(const wchar_t *in, wchar_t *out, size_t cap);
/* %USERPROFILE%\.local\bin\claude.exe, else claude.exe found by SearchPathW. Returns 1 when found. */
int ctu_renew_find_claude(wchar_t *out, DWORD cap);
/* Runs cmdline hidden (CREATE_NO_WINDOW, standard handles on NUL, filtered environment) in a job object that
 * ends the process tree when it closes. Waits for the exit, at most timeout_ms, or until stop (may be NULL)
 * is set; then ends the job. Returns RENEW_*. */
int ctu_renew_run(const wchar_t *cmdline, const wchar_t *workdir, HANDLE stop, DWORD timeout_ms);
/* The engine's renew hook (ctu_renew_fn); ctx = ctu_renew_ctx *. Returns at once when claude.exe is not found. */
void ctu_renew_hook(void *ctx);

#endif
