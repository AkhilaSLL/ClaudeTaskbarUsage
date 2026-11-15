/* The usage request over WinHTTP. HTTPS with the default certificate checks, no redirects
 * (the token goes to api.anthropic.com only), 15 s timeouts. */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

#define USAGE_HOST L"api.anthropic.com"
#define USAGE_PATH L"/api/oauth/usage"
#define USER_AGENT L"ClaudeTaskbarUsage/1.0"
#define TIMEOUT_MS 15000
#define MAX_BODY (1024 * 1024)

static void reason(ctu_http_result *res, const char *what, DWORD code)
{
    const char *txt;
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: txt = "timeout"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: txt = "host name not resolved (no network?)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: txt = "cannot connect"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: txt = "connection reset"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: txt = "certificate check failed"; break;
    default: txt = NULL; break;
    }
    if (txt)
        snprintf(res->err, sizeof(res->err), "%s", txt);
    else
        snprintf(res->err, sizeof(res->err), "%s error %lu", what, (unsigned long)code);
}

void ctu_http_fetch(void *ctx, const char *token, ctu_http_result *res)
{
    (void)ctx;
    res->status = 0;
    res->body = NULL;
    res->len = 0;
    res->err[0] = 0;

    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t *hdr = NULL;
    size_t hdrcap = 0;

    ses = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) /* Before Windows 8.1 the automatic proxy type does not exist. */
        ses = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        reason(res, "WinHttpOpen", GetLastError());
        goto done;
    }
    WinHttpSetTimeouts(ses, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS);
    con = WinHttpConnect(ses, USAGE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        reason(res, "WinHttpConnect", GetLastError());
        goto done;
    }
    req = WinHttpOpenRequest(con, L"GET", USAGE_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) {
        reason(res, "WinHttpOpenRequest", GetLastError());
        goto done;
    }
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));

    /* Headers. The token is ASCII; widen it into a buffer that is wiped afterwards. */
    size_t tlen = strlen(token);
    hdrcap = tlen + 200;
    hdr = (wchar_t *)calloc(hdrcap, sizeof(wchar_t));
    if (!hdr) {
        snprintf(res->err, sizeof(res->err), "out of memory");
        goto done;
    }
    size_t n = 0;
    const wchar_t *pre = L"Authorization: Bearer ";
    for (const wchar_t *p = pre; *p; p++) hdr[n++] = *p;
    for (size_t i = 0; i < tlen; i++) hdr[n++] = (wchar_t)(unsigned char)token[i];
    const wchar_t *post = L"\r\nanthropic-beta: oauth-2025-04-20\r\nContent-Type: application/json\r\n";
    for (const wchar_t *p = post; *p; p++) hdr[n++] = *p;
    hdr[n] = 0;

    if (!WinHttpSendRequest(req, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        reason(res, "WinHttpSendRequest", GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        reason(res, "WinHttpReceiveResponse", GetLastError());
        goto done;
    }
    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &slen, WINHTTP_NO_HEADER_INDEX)) {
        reason(res, "WinHttpQueryHeaders", GetLastError());
        goto done;
    }

    size_t cap = 8192, len = 0;
    char *body = (char *)malloc(cap + 1);
    if (!body) {
        snprintf(res->err, sizeof(res->err), "out of memory");
        goto done;
    }
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            reason(res, "WinHttpQueryDataAvailable", GetLastError());
            free(body);
            goto done;
        }
        if (avail == 0) break;
        if (len + avail > MAX_BODY) {
            snprintf(res->err, sizeof(res->err), "answer too large");
            free(body);
            goto done;
        }
        if (len + avail > cap) {
            while (len + avail > cap) cap *= 2;
            char *nb = (char *)realloc(body, cap + 1);
            if (!nb) {
                free(body);
                snprintf(res->err, sizeof(res->err), "out of memory");
                goto done;
            }
            body = nb;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, body + len, avail, &got)) {
            reason(res, "WinHttpReadData", GetLastError());
            free(body);
            goto done;
        }
        if (got == 0) break;
        len += got;
    }
    body[len] = 0;
    res->body = body;
    res->len = len;
    res->status = (int)status;

done:
    if (hdr) {
        SecureZeroMemory(hdr, hdrcap * sizeof(wchar_t));
        free(hdr);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
}
