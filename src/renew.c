/* Login renewal through the Claude CLI ("claude.exe mcp list", hidden, in a job object). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renew.h"

size_t ctu_env_block_len(const wchar_t *block)
{
    if (!block) return 0;
    const wchar_t *p = block;
    while (*p) p += wcslen(p) + 1;
    return (size_t)(p - block) + 1;
}

/* Started from a Claude desktop session, the CLI gets host authentication variables and may then not write
 * the credentials file. */
static int env_removed(const wchar_t *entry)
{
    return _wcsnicmp(entry, L"CLAUDE", 6) == 0 || _wcsnicmp(entry, L"ANTHROPIC", 9) == 0;
}

size_t ctu_env_filter(const wchar_t *in, wchar_t *out, size_t cap)
{
    size_t need = ctu_env_block_len(in) + 1;
    if (!in || !out || cap < need) return 0;
    size_t n = 0;
    for (const wchar_t *p = in; *p; p += wcslen(p) + 1) {
        size_t len = wcslen(p) + 1;
        if (env_removed(p)) continue;
        memcpy(out + n, p, len * sizeof(wchar_t));
        n += len;
    }
    if (n == 0) out[n++] = 0; /* an empty block: two NULs */
    out[n++] = 0;
    return n;
}

int ctu_renew_find_claude(wchar_t *out, DWORD cap)
{
    wchar_t home[CTU_PATH_MAX];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, CTU_PATH_MAX);
    if (n > 0 && n < CTU_PATH_MAX) {
        _snwprintf(out, cap, L"%ls\\.local\\bin\\claude.exe", home);
        out[cap - 1] = 0;
        DWORD a = GetFileAttributesW(out);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    n = SearchPathW(NULL, L"claude.exe", NULL, cap, out, NULL);
    if (n > 0 && n < cap) return 1;
    out[0] = 0;
    return 0;
}

int ctu_renew_run(const wchar_t *cmdline, const wchar_t *workdir, HANDLE stop, DWORD timeout_ms)
{
    if (!cmdline || !cmdline[0]) return RENEW_NOT_STARTED;
    /* CreateProcessW may write into the command line. */
    size_t clen = wcslen(cmdline) + 1;
    wchar_t *cmd = (wchar_t *)malloc(clen * sizeof(wchar_t));
    if (!cmd) return RENEW_NOT_STARTED;
    memcpy(cmd, cmdline, clen * sizeof(wchar_t));

    /* The widget's own environment without CLAUDE* and ANTHROPIC*; never the unfiltered one. */
    wchar_t *env = NULL;
    LPWCH cur = GetEnvironmentStringsW();
    if (cur) {
        size_t cap = ctu_env_block_len(cur) + 2;
        env = (wchar_t *)malloc(cap * sizeof(wchar_t));
        if (env && !ctu_env_filter(cur, env, cap)) {
            free(env);
            env = NULL;
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!env) {
        free(cmd);
        return RENEW_NOT_STARTED;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        free(env);
        free(cmd);
        return RENEW_NOT_STARTED;
    }

    /* The job holds the process tree (the CLI starts MCP server processes); closing it ends them all. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
            CloseHandle(job);
            job = NULL;
        }
    }

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                             env, (workdir && workdir[0]) ? workdir : NULL, &si, &pi);
    CloseHandle(nul);
    free(env);
    free(cmd);
    if (!ok) {
        if (job) CloseHandle(job);
        return RENEW_NOT_STARTED;
    }
    /* Assigned while suspended, so that every child process starts inside the job. */
    int in_job = job && AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    HANDLE hs[2] = { pi.hProcess, stop };
    DWORD r = WaitForMultipleObjects(stop ? 2 : 1, hs, FALSE, timeout_ms);
    int rc = r == WAIT_OBJECT_0 ? RENEW_EXITED : (stop && r == WAIT_OBJECT_0 + 1) ? RENEW_STOPPED : RENEW_TIMEOUT;
    if (rc != RENEW_EXITED) {
        if (in_job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job); /* KILL_ON_JOB_CLOSE: also ends what the CLI left running */
    return rc;
}

void ctu_renew_hook(void *ctx)
{
    ctu_renew_ctx *c = (ctu_renew_ctx *)ctx;
    if (!c) return;
    wchar_t cmd[CTU_RENEW_CMD_MAX + 16];
    if (c->cmd[0]) {
        wcsncpy(cmd, c->cmd, CTU_RENEW_CMD_MAX + 15);
        cmd[CTU_RENEW_CMD_MAX + 15] = 0;
    } else {
        wchar_t exe[CTU_PATH_MAX];
        if (!ctu_renew_find_claude(exe, CTU_PATH_MAX)) return;
        _snwprintf(cmd, CTU_RENEW_CMD_MAX + 16, L"\"%ls\" mcp list", exe);
        cmd[CTU_RENEW_CMD_MAX + 15] = 0;
    }
    ctu_renew_run(cmd, c->workdir, c->stop, CTU_RENEW_TIMEOUT_MS);
}
