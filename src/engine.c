/* Data step: Claude Code cache, credentials, request rules, backoff, saved data. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define MAX_FILE_BYTES (64u * 1024u * 1024u)

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    if (!dstlen) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void append(char *out, size_t outlen, const char *fmt, ...)
{
    size_t n = strlen(out);
    if (n + 1 >= outlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, outlen - n, fmt, ap);
    va_end(ap);
}

static long long ft_to_ll(FILETIME ft)
{
    return (long long)(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

long long ctu_file_mtime(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!path || !path[0] || !GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return -1;
    return ft_to_ll(fa.ftLastWriteTime);
}

char *ctu_read_file(const wchar_t *path, size_t *len, long long *mtime)
{
    if (len) *len = 0;
    if (!path || !path[0]) return NULL;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    FILETIME wt;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > MAX_FILE_BYTES || !GetFileTime(h, NULL, NULL, &wt)) {
        CloseHandle(h);
        return NULL;
    }
    DWORD n = (DWORD)sz.QuadPart;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    DWORD got = 0, total = 0;
    while (total < n) {
        if (!ReadFile(h, buf + total, n - total, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    buf[total] = 0;
    if (len) *len = total;
    if (mtime) *mtime = ft_to_ll(wt);
    return buf;
}

static void free_secure(char *buf, size_t len)
{
    if (!buf) return;
    SecureZeroMemory(buf, len);
    free(buf);
}

void engine_init(ctu_engine *e, int interval_min)
{
    memset(e, 0, sizeof(*e));
    e->v.interval_min = interval_min;
    backoff_init(&e->v.bo);
    e->cc_mtime = -1;
    e->ext_seen_mtime = -1;
    e->ext_seen_size = -1;
    e->ext_seen_lbl_mtime = -1;
    e->login_cred_mtime = -1;
    e->login_seen_mtime = -1;
    e->use_cc_cache = 1;
    e->live_source = SRC_LIVE;
}

static long long interval_ms(const ctu_engine *e)
{
    int m = e->v.interval_min;
    if (m < 5) m = 5;
    return (long long)m * CTU_MIN_MS;
}

/* ---- last.json ---------------------------------------------------------------------------- */

static void json_escape_append(char *out, size_t outlen, const char *s)
{
    append(out, outlen, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            append(out, outlen, "\\%c", c);
        else if (c < 0x20)
            append(out, outlen, "\\u%04x", c);
        else
            append(out, outlen, "%c", c);
    }
    append(out, outlen, "\"");
}

static void window_json(char *out, size_t outlen, const uwin *w, int first)
{
    if (!w->present) return;
    if (!first) append(out, outlen, ",");
    json_escape_append(out, outlen, w->key);
    append(out, outlen, ":{\"utilization\":%d,\"resets_at\":", w->pct);
    if (w->reset_iso[0])
        json_escape_append(out, outlen, w->reset_iso);
    else
        append(out, outlen, "null");
    append(out, outlen, ",\"name\":");
    json_escape_append(out, outlen, w->name);
    append(out, outlen, "}");
}

static const char *source_name(int s)
{
    switch (s) {
    case SRC_SAVED: return "saved";
    case SRC_CACHE: return "cache";
    case SRC_LIVE: return "live";
    case SRC_SAMPLE: return "sample";
    case SRC_EXT: return "extension";
    default: return "none";
    }
}

/* The "source" value of last.json back to SRC_*; unknown or missing values (older files) give SRC_NONE. */
static int source_from_name(const char *s)
{
    if (!s) return SRC_NONE;
    if (strcmp(s, "live") == 0) return SRC_LIVE;
    if (strcmp(s, "cache") == 0) return SRC_CACHE;
    if (strcmp(s, "sample") == 0) return SRC_SAMPLE;
    if (strcmp(s, "extension") == 0) return SRC_EXT;
    return SRC_NONE;
}

static void engine_save(const ctu_engine *e)
{
    if (!e->last_path[0] || !e->v.have_data) return;
    static char buf[16384];
    buf[0] = 0;
    append(buf, sizeof(buf), "{\"fetchedAtMs\":%lld,\"source\":\"%s\",\"utilization\":{", e->v.data_ms,
           source_name(e->v.origin != SRC_NONE ? e->v.origin : e->v.source));
    int first = 1;
    const usage_t *u = &e->v.data;
    if (u->five.present) {
        window_json(buf, sizeof(buf), &u->five, first);
        first = 0;
    }
    if (u->seven.present) {
        window_json(buf, sizeof(buf), &u->seven, first);
        first = 0;
    }
    for (int i = 0; i < u->nextra; i++) {
        window_json(buf, sizeof(buf), &u->extra[i], first);
        first = 0;
    }
    append(buf, sizeof(buf), "}}\r\n");

    wchar_t tmp[CTU_PATH_MAX + 8];
    _snwprintf(tmp, CTU_PATH_MAX + 8, L"%ls.tmp", e->last_path);
    tmp[CTU_PATH_MAX + 7] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = (DWORD)strlen(buf), wr = 0;
    BOOL ok = WriteFile(h, buf, n, &wr, NULL) && wr == n;
    CloseHandle(h);
    if (ok)
        MoveFileExW(tmp, e->last_path, MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileW(tmp);
}

/* Parses { fetchedAtMs, utilization } (our last.json and Claude Code's cachedUsageUtilization). */
static int parse_cached(const jval *obj, usage_t *u, long long *fetched)
{
    const jval *fa = json_get(obj, "fetchedAtMs");
    const jval *ut = json_get(obj, "utilization");
    if (!json_is_num(fa) || !json_is_obj(ut)) return 0;
    if (!usage_parse(ut, u, NULL, 0, NULL, 0)) return 0;
    *fetched = (long long)fa->num;
    return 1;
}

int engine_load_saved(ctu_engine *e)
{
    size_t len = 0;
    char *buf = ctu_read_file(e->last_path, &len, NULL);
    if (!buf) return 0;
    jdoc doc = { 0 };
    jval *root = json_parse(&doc, buf, len, NULL, 0);
    usage_t u;
    long long fetched = 0;
    int ok = root && parse_cached(root, &u, &fetched);
    if (ok) {
        e->v.data = u;
        e->v.have_data = 1;
        e->v.data_ms = fetched;
        e->v.source = SRC_SAVED;
        e->v.origin = source_from_name(json_str(json_get(root, "source")));
    }
    json_free(&doc);
    free(buf);
    return ok;
}

/* Clears the note after new data from a shared source; a rate-limit note stays while its backoff runs. */
static void shared_adopted(ctu_engine *e, long long now_ms)
{
    e->v.stale = 0;
    if (e->v.note_kind != NOTE_RATE || now_ms >= e->v.bo.until_ms) {
        e->v.note_kind = NOTE_NONE;
        e->v.note[0] = 0;
    }
    engine_save(e);
}

/* ---- Claude Code's cache ------------------------------------------------------------------ */

int engine_check_cache(ctu_engine *e, long long now_ms)
{
    if (!e->use_cc_cache || !e->ccjson_path[0]) return 0;
    e->cc_checked_ms = now_ms;
    long long mt = ctu_file_mtime(e->ccjson_path);
    if (mt < 0 || mt == e->cc_mtime) return 0;
    size_t len = 0;
    long long mt2 = 0;
    char *buf = ctu_read_file(e->ccjson_path, &len, &mt2);
    if (!buf) return 0;
    jdoc doc = { 0 };
    jval *root = json_parse(&doc, buf, len, NULL, 0);
    int adopted = 0;
    if (root) {
        /* Remember the time only after a good parse, so that a half-written file is read again. */
        e->cc_mtime = mt2;
        const jval *c = json_get(root, "cachedUsageUtilization");
        usage_t u;
        long long fetched = 0;
        if (json_is_obj(c) && parse_cached(c, &u, &fetched) && ctu_cache_is_newer(fetched, e->v.data_ms) &&
            (u.five.present || u.seven.present)) {
            e->v.data = u;
            e->v.have_data = 1;
            e->v.data_ms = fetched;
            e->v.source = SRC_CACHE;
            e->v.origin = SRC_CACHE;
            adopted = 1;
            shared_adopted(e, now_ms);
        }
    }
    json_free(&doc);
    free(buf);
    return adopted;
}

/* ---- the VS Code usage extension's records ------------------------------------------------ */

void engine_default_ext_dirs(ctu_engine *e, const wchar_t *appdata)
{
    static const wchar_t *const editors[] = { L"Code", L"Code - Insiders", L"VSCodium", L"Cursor", L"Antigravity" };
    e->n_ext_dirs = 0;
    if (!appdata || !appdata[0]) return;
    for (size_t i = 0; i < sizeof(editors) / sizeof(editors[0]) && e->n_ext_dirs < CTU_EXT_MAX_DIRS; i++) {
        wchar_t *d = e->ext_dirs[e->n_ext_dirs++];
        _snwprintf(d, CTU_PATH_MAX, L"%ls\\%ls\\User\\globalStorage\\growthjack.claude-code-usage", appdata,
                   editors[i]);
        d[CTU_PATH_MAX - 1] = 0;
    }
}

static int file_info(const wchar_t *path, long long *mtime, long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    *mtime = ft_to_ll(fa.ftLastWriteTime);
    *size = (long long)(((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    return 1;
}

int engine_check_ext(ctu_engine *e, long long now_ms)
{
    if (e->n_ext_dirs <= 0) return 0;
    /* The candidate file with the newest modified time wins (the first one on a tie). */
    int best = -1;
    long long best_mt = 0, best_sz = 0;
    wchar_t path[CTU_PATH_MAX + 64];
    for (int i = 0; i < e->n_ext_dirs && i < CTU_EXT_MAX_DIRS; i++) {
        long long mt, sz;
        _snwprintf(path, CTU_PATH_MAX + 64, L"%ls\\%ls", e->ext_dirs[i], CTU_EXT_FILE);
        path[CTU_PATH_MAX + 63] = 0;
        if (file_info(path, &mt, &sz) && (best < 0 || mt > best_mt)) {
            best = i;
            best_mt = mt;
            best_sz = sz;
        }
    }
    if (best < 0) return 0;
    wchar_t lbl[CTU_PATH_MAX + 64];
    _snwprintf(path, CTU_PATH_MAX + 64, L"%ls\\%ls", e->ext_dirs[best], CTU_EXT_FILE);
    path[CTU_PATH_MAX + 63] = 0;
    _snwprintf(lbl, CTU_PATH_MAX + 64, L"%ls\\%ls", e->ext_dirs[best], CTU_EXT_LABELS);
    lbl[CTU_PATH_MAX + 63] = 0;
    long long lbl_mt = ctu_file_mtime(lbl);
    /* Read again only when the file, its time, its size or the labels file changed. */
    if (wcscmp(path, e->ext_seen_path) == 0 && best_mt == e->ext_seen_mtime && best_sz == e->ext_seen_size &&
        lbl_mt == e->ext_seen_lbl_mtime)
        return 0;
    wcsncpy(e->ext_seen_path, path, CTU_PATH_MAX - 1);
    e->ext_seen_path[CTU_PATH_MAX - 1] = 0;
    e->ext_seen_mtime = best_mt;
    e->ext_seen_size = best_sz;
    e->ext_seen_lbl_mtime = lbl_mt;

    char label[40];
    int have_label = 0;
    if (lbl_mt >= 0) {
        size_t llen = 0;
        char *ltext = ctu_read_file(lbl, &llen, NULL);
        if (ltext && llen <= 4096) have_label = ctu_ext_label(ltext, llen, label, sizeof(label));
        free(ltext);
    }
    size_t len = 0;
    char *tail = ctu_read_tail(path, CTU_EXT_TAIL_BYTES, &len);
    if (!tail) return 0;
    usage_t u;
    long long observed = 0;
    int ok = ctu_ext_build(tail, len, have_label ? label : NULL, now_ms, &u, &observed);
    free(tail);
    if (!ok || !ctu_cache_is_newer(observed, e->v.data_ms)) return 0;
    e->v.data = u; /* the whole set: no window of an older set is carried over */
    e->v.have_data = 1;
    e->v.data_ms = observed;
    e->v.source = SRC_EXT;
    e->v.origin = SRC_EXT;
    shared_adopted(e, now_ms);
    return 1;
}

/* ---- the step ----------------------------------------------------------------------------- */

/* Login retry due: NOTE_LOGIN and the credentials file changed since the step that set the note
 * (missing to present counts as a change). Uses the time seen by the last step: no file access here. */
static int login_retry_due(const ctu_engine *e)
{
    return e->v.note_kind == NOTE_LOGIN && !e->skip_credentials && e->login_seen_mtime != e->login_cred_mtime;
}

long long engine_next_due(const ctu_engine *e)
{
    if (login_retry_due(e)) {
        /* The interval does not delay the retry; the 5 min minimum (as in engine_step) and the backoff do. */
        long long last_fetch = e->v.last_request_ms > e->v.data_ms ? e->v.last_request_ms : e->v.data_ms;
        long long due = last_fetch > 0 ? last_fetch + CTU_MIN_REQUEST_MS : 0;
        if (e->v.bo.until_ms > due) due = e->v.bo.until_ms;
        return due;
    }
    long long base = e->v.last_attempt_ms > e->v.data_ms ? e->v.last_attempt_ms : e->v.data_ms;
    long long due = base + interval_ms(e);
    long long minreq = e->v.last_request_ms + CTU_MIN_REQUEST_MS;
    if (minreq > due) due = minreq;
    if (e->v.bo.until_ms > due) due = e->v.bo.until_ms;
    return due;
}

static void clock_of(long long ms, char *out, size_t outlen)
{
    int h, mi;
    if (ctu_local_parts(ms, NULL, NULL, NULL, &h, &mi, NULL))
        ctu_clock_text(h, mi, 0, 0, out, outlen);
    else
        copy_str(out, outlen, "?");
}

static void set_note(ctu_engine *e, int kind, const char *text)
{
    e->v.note_kind = kind;
    copy_str(e->v.note, sizeof(e->v.note), text);
    if (kind != NOTE_NONE) e->v.stale = 1;
}

/* NOTE_LOGIN with the credentials file time taken before the read: the next retry needs a further change. */
static void set_login_note(ctu_engine *e, long long cred_mt)
{
    set_note(e, NOTE_LOGIN, CTU_LOGIN_NOTE);
    e->login_cred_mtime = cred_mt;
    e->login_seen_mtime = cred_mt;
}

int engine_step(ctu_engine *e, long long now_ms, int force)
{
    int changed = engine_check_cache(e, now_ms);
    changed |= engine_check_ext(e, now_ms);
    /* While NOTE_LOGIN: one attribute call on the credentials file; a change makes a login retry due. */
    long long cred_mt = -1;
    int have_cred_mt = 0;
    if (e->v.note_kind == NOTE_LOGIN && !e->skip_credentials) {
        cred_mt = ctu_file_mtime(e->cred_path);
        have_cred_mt = 1;
        e->login_seen_mtime = cred_mt;
    }
    if (!force && now_ms < engine_next_due(e)) return changed;

    e->v.hint[0] = 0;
    char clock[32];
    if (now_ms < e->v.bo.until_ms) {
        clock_of(e->v.bo.until_ms, clock, sizeof(clock));
        if (force) snprintf(e->v.hint, sizeof(e->v.hint), "Refresh now: rate limited until %s.", clock);
        return 1;
    }
    long long last_fetch = e->v.last_request_ms > e->v.data_ms ? e->v.last_request_ms : e->v.data_ms;
    if (last_fetch > 0 && now_ms - last_fetch < CTU_MIN_REQUEST_MS) {
        clock_of(last_fetch + CTU_MIN_REQUEST_MS, clock, sizeof(clock));
        if (force) snprintf(e->v.hint, sizeof(e->v.hint), "Refresh now: next request allowed at %s.", clock);
        return 1;
    }

    e->v.last_attempt_ms = now_ms;
    char token[8192];
    token[0] = 0;
    if (!e->skip_credentials) {
        /* The time is taken before the read, so that a write during the read is seen as a change later. */
        if (!have_cred_mt) cred_mt = ctu_file_mtime(e->cred_path);
        size_t len = 0;
        char *cred = ctu_read_file(e->cred_path, &len, NULL);
        int rc = ctu_cred_check(cred, len, now_ms, token, sizeof(token));
        free_secure(cred, len);
        if (rc != CRED_OK) {
            SecureZeroMemory(token, sizeof(token));
            e->v.own_429 = 0;
            set_login_note(e, cred_mt);
            return 1;
        }
    }

    e->v.last_request_ms = now_ms;
    e->v.requests_made++;
    ctu_http_result res;
    memset(&res, 0, sizeof(res));
    if (e->fetch) {
        e->fetch(e->fetch_ctx, token, &res);
    } else {
        copy_str(res.err, sizeof(res.err), "no fetch function");
    }
    SecureZeroMemory(token, sizeof(token));

    char msg[256];
    e->v.own_429 = 0;
    if (res.status == 0) {
        snprintf(msg, sizeof(msg), "Request failed: %s", res.err[0] ? res.err : "no answer");
        set_note(e, NOTE_FAIL, msg);
        free(res.body);
        return 1;
    }

    jdoc doc = { 0 };
    jval *root = res.body ? json_parse(&doc, res.body, res.len, NULL, 0) : NULL;
    usage_t u;
    char err[160], etype[64];
    int ok = root ? usage_parse(root, &u, err, sizeof(err), etype, sizeof(etype)) : 0;
    if (!root) {
        copy_str(err, sizeof(err), "the answer is not JSON");
        etype[0] = 0;
    }

    if (res.status == 429 || strcmp(etype, "rate_limit_error") == 0) {
        backoff_on_429(&e->v.bo, now_ms);
        e->v.own_429 = 1;
        clock_of(e->v.bo.until_ms, clock, sizeof(clock));
        snprintf(msg, sizeof(msg), "Rate limited until %s.", clock);
        set_note(e, NOTE_RATE, msg);
    } else if (res.status == 401 || strcmp(etype, "authentication_error") == 0) {
        set_login_note(e, cred_mt);
    } else if (res.status != 200 || !ok) {
        if (res.status != 200)
            snprintf(msg, sizeof(msg), "Request failed: HTTP %d%s%.100s", res.status, err[0] ? ", " : "", err);
        else
            snprintf(msg, sizeof(msg), "Request failed: %.150s", err);
        set_note(e, NOTE_FAIL, msg);
    } else {
        e->v.data = u;
        e->v.have_data = 1;
        e->v.data_ms = now_ms;
        e->v.source = e->live_source;
        e->v.origin = e->live_source;
        e->v.stale = 0;
        set_note(e, NOTE_NONE, "");
        backoff_reset(&e->v.bo);
        engine_save(e);
    }
    json_free(&doc);
    free(res.body);
    return 1;
}

int ctu_dim_rule(int origin, int stale, long long age_ms, int interval_min, int own_429)
{
    int m = interval_min < 5 ? 5 : interval_min;
    int old = age_ms > 2LL * m * CTU_MIN_MS + CTU_MIN_MS;
    /* The extension rewrites its file only when a value changed, so its age alone does not show stale data;
     * an own 429 shows that another client still polls with success. */
    if (origin == SRC_EXT) return old && !own_429;
    return stale || old;
}

int ctu_view_dimmed(const ctu_view *v, long long now_ms)
{
    if (!v->have_data) return 0;
    return ctu_dim_rule(v->origin, v->stale, now_ms - v->data_ms, v->interval_min, v->own_429);
}

/* ---- texts -------------------------------------------------------------------------------- */

const char *ctu_source_suffix(const ctu_view *v)
{
    int s = v->source == SRC_SAVED ? v->origin : v->source;
    switch (s) {
    case SRC_LIVE: return " (live)";
    case SRC_CACHE: return " (Claude Code cache)";
    case SRC_EXT: return " (VS Code usage extension)";
    case SRC_SAMPLE: return " (sample file)";
    default: return "";
    }
}

const char *ctu_view_note(const ctu_view *v)
{
    if (v->note_kind == NOTE_RATE && v->have_data && v->origin == SRC_EXT) return CTU_EXT_RATE_NOTE;
    return v->note;
}

static void window_line(char *out, size_t outlen, const uwin *w, long long now_ms, int show_remaining)
{
    if (!w->present) {
        append(out, outlen, "%s: no data\r\n", w->name);
        return;
    }
    char reset[96];
    ctu_reset_text(w->reset_ms, now_ms, reset, sizeof(reset));
    int left = 100 - w->pct;
    if (left < 0) left = 0;
    if (show_remaining)
        append(out, outlen, "%s: %d%% left (%d%% used)%s%s\r\n", w->name, left, w->pct, reset[0] ? ", " : "", reset);
    else
        append(out, outlen, "%s: %d%% used%s%s\r\n", w->name, w->pct, reset[0] ? ", " : "", reset);
}

void ctu_build_tooltip(const ctu_view *v, long long now_ms, int show_remaining, char *out, size_t outlen)
{
    out[0] = 0;
    if (!v->have_data) {
        append(out, outlen, "Claude usage: no data yet\r\n");
    } else {
        window_line(out, outlen, &v->data.five, now_ms, show_remaining);
        window_line(out, outlen, &v->data.seven, now_ms, show_remaining);
        for (int i = 0; i < v->data.nextra; i++) window_line(out, outlen, &v->data.extra[i], now_ms, show_remaining);
        long long age = now_ms - v->data_ms;
        const char *from = ctu_source_suffix(v);
        if (age < CTU_MIN_MS) {
            append(out, outlen, "Updated just now%s\r\n", from);
        } else {
            char span[32];
            ctu_span_text(age, span, sizeof(span));
            append(out, outlen, "Updated %s ago%s\r\n", span, from);
        }
    }
    const char *note = ctu_view_note(v);
    if (note[0]) append(out, outlen, "%s\r\n", note);
    if (v->hint[0]) append(out, outlen, "%s\r\n", v->hint);
    size_t n = strlen(out);
    while (n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n') {
        out[n - 2] = 0;
        n -= 2;
    }
}

static void once_window(char *out, size_t outlen, const char *label, const uwin *w, long long now_ms)
{
    if (!w->present) {
        append(out, outlen, "%s=-- | ", label);
        return;
    }
    char reset[96];
    ctu_reset_text(w->reset_ms, now_ms, reset, sizeof(reset));
    append(out, outlen, "%s=%d%%", label, w->pct);
    if (w->reset_iso[0]) append(out, outlen, " resets_at=%s [%s]", w->reset_iso, reset);
    append(out, outlen, " | ");
}

void ctu_build_once_line(const ctu_view *v, long long now_ms, char *out, size_t outlen)
{
    out[0] = 0;
    if (!v->have_data) {
        append(out, outlen, "error: %s", v->note[0] ? v->note : "no data");
        if (v->hint[0]) append(out, outlen, " | %s", v->hint);
        return;
    }
    once_window(out, outlen, "5h", &v->data.five, now_ms);
    once_window(out, outlen, "W", &v->data.seven, now_ms);
    for (int i = 0; i < v->data.nextra; i++) once_window(out, outlen, v->data.extra[i].name, &v->data.extra[i], now_ms);
    int y, mo, d, h, mi;
    char when[40] = "?";
    if (ctu_local_parts(v->data_ms, &y, &mo, &d, &h, &mi, NULL))
        snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    append(out, outlen, "source=%s fetched=%s", source_name(v->source), when);
    if (v->note[0]) append(out, outlen, " | note=%s", ctu_view_note(v));
    if (v->hint[0]) append(out, outlen, " | %s", v->hint);
}

/* ---- sample fetch (test aid) -------------------------------------------------------------- */

void ctu_sample_fetch(void *ctx, const char *token, ctu_http_result *res)
{
    (void)token;
    const wchar_t *path = (const wchar_t *)ctx;
    size_t len = 0;
    res->body = ctu_read_file(path, &len, NULL);
    res->len = len;
    if (!res->body) {
        res->status = 0;
        copy_str(res->err, sizeof(res->err), "sample file not readable");
        return;
    }
    res->status = 200;
    wchar_t st[16];
    DWORD n = GetEnvironmentVariableW(L"CTU_SAMPLE_STATUS", st, 16);
    if (n > 0 && n < 16) res->status = _wtoi(st);
}
