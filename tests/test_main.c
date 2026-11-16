/* Console tests for the data logic (JSON, usage parsing, rules, texts, backoff, engine).
 * Usage: ctu_tests.exe <samples dir> <scratch dir> */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/engine.h"
#include "../src/ext.h"
#include "../src/json.h"
#include "../src/logo.h"
#include "../src/logo_mask.h"
#include "../src/render.h"
#include "../src/renew.h"
#include "../src/text_d2d.h"
#include "../src/usage.h"

static int g_fail = 0, g_pass = 0;
static wchar_t g_samples[MAX_PATH], g_scratch[MAX_PATH];

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) {                                                                 \
            g_pass++;                                                               \
        } else {                                                                    \
            g_fail++;                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                           \
    } while (0)

#define CHECK_STR(a, b)                                                             \
    do {                                                                            \
        const char *a_ = (a), *b_ = (b);                                            \
        if (a_ && b_ && strcmp(a_, b_) == 0) {                                      \
            g_pass++;                                                               \
        } else {                                                                    \
            g_fail++;                                                               \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,           \
                   a_ ? a_ : "(null)", b_ ? b_ : "(null)");                         \
        }                                                                           \
    } while (0)

static jval *parse(jdoc *d, const char *s) { return json_parse(d, s, strlen(s), NULL, 0); }

static char *read_sample(const char *name, size_t *len)
{
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\%hs", g_samples, name);
    char *b = ctu_read_file(p, len, NULL);
    if (!b) printf("cannot read sample %s\n", name);
    return b;
}

static int parse_sample(const char *name, usage_t *u, char *err, char *etype)
{
    size_t len = 0;
    char *b = read_sample(name, &len);
    if (!b) return -1;
    jdoc d = { 0 };
    jval *r = json_parse(&d, b, len, NULL, 0);
    int ok = r ? usage_parse(r, u, err, 160, etype, 64) : -2;
    json_free(&d);
    free(b);
    return ok;
}

/* Independent oracle for epoch ms of a UTC date. */
static long long utc_ms(int y, int mo, int d, int h, int mi, int s, int ms)
{
    SYSTEMTIME st = { 0 };
    st.wYear = (WORD)y;
    st.wMonth = (WORD)mo;
    st.wDay = (WORD)d;
    st.wHour = (WORD)h;
    st.wMinute = (WORD)mi;
    st.wSecond = (WORD)s;
    st.wMilliseconds = (WORD)ms;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)((t - 116444736000000000ULL) / 10000ULL);
}

static void test_json(void)
{
    jdoc d = { 0 };
    char err[128];
    const char *s = "{\"s\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\\u00e9\\ud83d\\ude00\\u20ac\"}";
    jval *r = json_parse(&d, s, strlen(s), err, sizeof(err));
    CHECK(r != NULL);
    const char *v = json_str(json_get(r, "s"));
    CHECK_STR(v, "a\"b\\c/d\b\f\n\r\t\xC3\xA9\xF0\x9F\x98\x80\xE2\x82\xAC");
    json_free(&d);

    r = parse(&d, " { \"a\" : [ 1 , { \"b\" : [ [ ] , { } ] } ] , \"c\" : null , \"t\":true, \"f\":false } ");
    CHECK(r != NULL && r->type == J_OBJ && r->len == 4);
    const jval *a = json_get(r, "a");
    CHECK(json_is_arr(a) && a->len == 2);
    CHECK(a && a->child && json_num(a->child, -1) == 1);
    const jval *b = json_get(a && a->child ? a->child->next : NULL, "b");
    CHECK(json_is_arr(b) && b->len == 2 && json_is_arr(b->child) && json_is_obj(b->child->next));
    CHECK(json_get(r, "c") && json_get(r, "c")->type == J_NULL);
    CHECK(json_get(r, "t") && json_get(r, "t")->type == J_BOOL && json_get(r, "t")->boolean == 1);
    CHECK(json_get(r, "f") && json_get(r, "f")->boolean == 0);
    CHECK(json_get(r, "missing") == NULL);
    json_free(&d);

    r = parse(&d, "[24.7, -0.5, 1e2, 1.5E-3, 0, -12, 3.0]");
    CHECK(r && r->len == 7);
    double want[7] = { 24.7, -0.5, 100, 0.0015, 0, -12, 3 };
    int i = 0;
    for (const jval *c = r ? r->child : NULL; c; c = c->next, i++) CHECK(c->type == J_NUM && c->num == want[i]);
    json_free(&d);

    /* Errors. */
    const char *bad[] = { "{\"a\":1,}", "[1,2", "\"abc", "{\"a\" 1}", "[01]", "\"\\x\"", "tru", "{} x", "[1.]",
                          "\"\\u12G4\"", "", "[\"a\nb\"]" };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        jval *x = json_parse(&d, bad[k], strlen(bad[k]), err, sizeof(err));
        if (x) printf("FAIL: accepted bad JSON #%u\n", (unsigned)k);
        CHECK(x == NULL && err[0] != 0);
        json_free(&d);
    }

    /* Nesting: 300 levels are fine, 600 are refused (no stack overflow). */
    char deep[1300];
    for (int k = 0; k < 300; k++) deep[k] = '[';
    for (int k = 0; k < 300; k++) deep[300 + k] = ']';
    CHECK(json_parse(&d, deep, 600, NULL, 0) != NULL);
    json_free(&d);
    for (int k = 0; k < 600; k++) deep[k] = '[';
    for (int k = 0; k < 600; k++) deep[600 + k] = ']';
    CHECK(json_parse(&d, deep, 1200, NULL, 0) == NULL);
    json_free(&d);

    /* Large input (~900 KB): many members, nested arrays, strings with escapes. */
    size_t cap = 1200000, n = 0;
    char *big = (char *)malloc(cap);
    n += (size_t)sprintf(big + n, "{\"projects\":{");
    for (int k = 0; k < 9000; k++)
        n += (size_t)sprintf(big + n, "%s\"D:\\\\Proj\\\\p%05d\":{\"history\":[{\"display\":\"x\\n\\\"%d\\\"\",\"n\":%d.5}],\"ok\":true}",
                             k ? "," : "", k, k, k);
    n += (size_t)sprintf(big + n, "},\"cachedUsageUtilization\":{\"fetchedAtMs\":1762950000000,\"utilization\":{\"five_hour\":{\"utilization\":12.5,\"resets_at\":null}}}}");
    r = json_parse(&d, big, n, err, sizeof(err));
    CHECK(r != NULL);
    CHECK(n > 600000);
    const jval *pr = json_get(r, "projects");
    CHECK(pr && pr->len == 9000);
    const jval *last = json_get(pr, "D:\\Proj\\p08999");
    CHECK(last != NULL);
    const jval *hist = json_get(last, "history");
    CHECK(hist && hist->child && json_num(json_get(hist->child, "n"), 0) == 8999.5);
    CHECK_STR(json_str(json_get(hist ? hist->child : NULL, "display")), "x\n\"8999\"");
    CHECK(json_num(json_get(json_get(json_get(json_get(r, "cachedUsageUtilization"), "utilization"), "five_hour"), "utilization"), 0) == 12.5);
    json_free(&d);
    free(big);
}

static void test_usage(void)
{
    usage_t u;
    char err[160], et[64];
    CHECK(parse_sample("normal.json", &u, err, et) == 1);
    CHECK(u.five.present && u.five.pct == 24);
    CHECK(u.seven.present && u.seven.pct == 82);
    CHECK(u.five.reset_ms == utc_ms(2026, 11, 12, 15, 40, 0, 123));
    CHECK(u.seven.reset_ms == utc_ms(2026, 11, 15, 9, 0, 0, 0));
    CHECK(u.nextra == 1);
    CHECK_STR(u.extra[0].name, "Weekly Opus");
    CHECK(u.extra[0].pct == 3);

    CHECK(parse_sample("null_util.json", &u, err, et) == 1);
    CHECK(!u.five.present && u.seven.present && u.seven.pct == 41);
    char t[16];
    ctu_value_text(&u.five, 0, t, sizeof(t));
    CHECK_STR(t, "--");

    CHECK(parse_sample("no_seven_day.json", &u, err, et) == 1);
    CHECK(u.five.present && u.five.pct == 5 && !u.seven.present);
    CHECK(u.nextra == 1 && u.extra[0].pct == 7 && u.extra[0].reset_ms == 0);

    CHECK(parse_sample("orange_red.json", &u, err, et) == 1);
    CHECK(u.five.pct == 96 && u.seven.pct == 83 && u.nextra == 1 && u.extra[0].pct == 12);

    CHECK(parse_sample("error_rate_limit.json", &u, err, et) == 0);
    CHECK_STR(et, "rate_limit_error");
    CHECK_STR(err, "Rate limited. Please try again later.");
    CHECK(parse_sample("error_auth.json", &u, err, et) == 0);
    CHECK_STR(et, "authentication_error");

    CHECK(parse_sample("limits.json", &u, err, et) == 1);
    CHECK(u.five.pct == 51 && u.seven.pct == 6 && u.nextra == 1);
    CHECK_STR(u.extra[0].name, "Weekly Fable");

    jdoc d = { 0 };
    CHECK(usage_parse(parse(&d, "{}"), &u, err, sizeof(err), et, sizeof(et)) == 0);
    json_free(&d);
    CHECK(usage_parse(parse(&d, "[1]"), &u, err, sizeof(err), et, sizeof(et)) == 0);
    json_free(&d);
    CHECK(usage_parse(parse(&d, "{\"error\":{\"message\":\"boom\"}}"), &u, err, sizeof(err), et, sizeof(et)) == 0);
    CHECK_STR(err, "boom");
    json_free(&d);
    CHECK(usage_parse(parse(&d, "{\"seven_day_new_model\":{\"utilization\":44,\"resets_at\":null}}"), &u, err, sizeof(err), et, sizeof(et)) == 1);
    CHECK(u.nextra == 1);
    CHECK_STR(u.extra[0].name, "Weekly new model");
    json_free(&d);

    /* Floor and minimum. */
    const char *nums[] = { "-3", "79.99", "0.4", "80", "100.0", "null", "\"12\"" };
    int want[] = { 0, 79, 0, 80, 100, -1, -1 };
    for (int k = 0; k < 7; k++) {
        jval *x = parse(&d, nums[k]);
        int p = -1;
        int ok = usage_pct(x, &p);
        CHECK(ok ? p == want[k] : want[k] == -1);
        json_free(&d);
    }

    /* Threshold colour. */
    CHECK(ctu_level(0) == 0);
    CHECK(ctu_level(79) == 0);
    CHECK(ctu_level(80) == 1);
    CHECK(ctu_level(94) == 1);
    CHECK(ctu_level(95) == 2);
    CHECK(ctu_level(100) == 2);

    /* Show remaining. */
    uwin w = { 0 };
    w.present = 1;
    w.pct = 24;
    ctu_value_text(&w, 0, t, sizeof(t));
    CHECK_STR(t, "24%");
    ctu_value_text(&w, 1, t, sizeof(t));
    CHECK_STR(t, "76%");
    w.pct = 100;
    ctu_value_text(&w, 1, t, sizeof(t));
    CHECK_STR(t, "0%");
    w.pct = 104;
    ctu_value_text(&w, 1, t, sizeof(t));
    CHECK_STR(t, "0%");
    w.pct = 0;
    ctu_value_text(&w, 1, t, sizeof(t));
    CHECK_STR(t, "100%");
}

static void test_time(void)
{
    CHECK(iso8601_to_ms("2026-11-12T15:40:00Z") == utc_ms(2026, 11, 12, 15, 40, 0, 0));
    CHECK(iso8601_to_ms("2026-11-12T15:40:00.987654Z") == utc_ms(2026, 11, 12, 15, 40, 0, 987));
    CHECK(iso8601_to_ms("2026-11-12T17:40:00+02:00") == utc_ms(2026, 11, 12, 15, 40, 0, 0));
    CHECK(iso8601_to_ms("2026-11-12T10:10:00-05:30") == utc_ms(2026, 11, 12, 15, 40, 0, 0));
    CHECK(iso8601_to_ms("2024-02-29T00:00:00+00:00") == utc_ms(2024, 2, 29, 0, 0, 0, 0));
    CHECK(iso8601_to_ms("1970-01-01T00:00:00Z") == 0);
    CHECK(iso8601_to_ms("2026-13-01T00:00:00Z") == -1);
    CHECK(iso8601_to_ms("garbage") == -1);
    CHECK(iso8601_to_ms("2026-11-12T15:40:00Zjunk") == -1);
    CHECK(iso8601_to_ms(NULL) == -1);

    /* Local time: compare with the C runtime's conversion of the same instant. */
    long long ms[] = { utc_ms(2026, 11, 12, 15, 40, 0, 0), utc_ms(2026, 7, 1, 3, 5, 0, 0), utc_ms(2027, 3, 28, 1, 30, 0, 0) };
    for (int k = 0; k < 3; k++) {
        int y, mo, d, h, mi, wd;
        CHECK(ctu_local_parts(ms[k], &y, &mo, &d, &h, &mi, &wd));
        __time64_t tt = (__time64_t)(ms[k] / 1000);
        struct tm tmv;
        CHECK(_localtime64_s(&tmv, &tt) == 0);
        CHECK(y == tmv.tm_year + 1900 && mo == tmv.tm_mon + 1 && d == tmv.tm_mday && h == tmv.tm_hour &&
              mi == tmv.tm_min && wd == tmv.tm_wday);
    }

    char t[64];
    ctu_span_text((2 * 60 + 13) * 60000LL + 30000, t, sizeof(t));
    CHECK_STR(t, "2 h 13 min");
    ctu_span_text((2 * 1440 + 4 * 60 + 59) * 60000LL, t, sizeof(t));
    CHECK_STR(t, "2 d 4 h");
    ctu_span_text(45 * 60000LL, t, sizeof(t));
    CHECK_STR(t, "45 min");
    ctu_span_text(3 * 60 * 60000LL, t, sizeof(t));
    CHECK_STR(t, "3 h");
    ctu_span_text(1440 * 60000LL, t, sizeof(t));
    CHECK_STR(t, "1 d");
    ctu_span_text(30000, t, sizeof(t));
    CHECK_STR(t, "< 1 min");
    ctu_span_text(0, t, sizeof(t));
    CHECK_STR(t, "now");

    ctu_clock_text(15, 40, 4, 0, t, sizeof(t));
    CHECK_STR(t, "3:40 PM");
    ctu_clock_text(9, 0, 6, 1, t, sizeof(t));
    CHECK_STR(t, "Sat 9:00 AM");
    ctu_clock_text(0, 5, 0, 0, t, sizeof(t));
    CHECK_STR(t, "12:05 AM");
    ctu_clock_text(12, 0, 0, 0, t, sizeof(t));
    CHECK_STR(t, "12:00 PM");

    /* Reset text: "in 2 h 13 min" and the local clock of the reset time. */
    long long now = ctu_now_ms();
    long long reset = now + (2 * 60 + 13) * 60000LL + 20000;
    ctu_reset_text(reset, now, t, sizeof(t));
    CHECK(strstr(t, "(in 2 h 13 min)") != NULL);
    int h, mi;
    ctu_local_parts(reset, NULL, NULL, NULL, &h, &mi, NULL);
    char clock[32];
    ctu_clock_text(h, mi, 0, 0, clock, sizeof(clock));
    CHECK(strstr(t, clock) != NULL);
    reset = now + (2 * 1440 + 4 * 60 + 10) * 60000LL;
    ctu_reset_text(reset, now, t, sizeof(t));
    CHECK(strstr(t, "(in 2 d 4 h)") != NULL);
    /* Two days ahead is another local date, so the weekday is shown. */
    CHECK(strstr(t, "Sun ") || strstr(t, "Mon ") || strstr(t, "Tue ") || strstr(t, "Wed ") || strstr(t, "Thu ") ||
          strstr(t, "Fri ") || strstr(t, "Sat "));
    ctu_reset_text(0, now, t, sizeof(t));
    CHECK_STR(t, "");
}

static void test_rules(void)
{
    backoff_t b;
    backoff_init(&b);
    long long now = 1000000;
    long long seq[] = { 5, 10, 20, 30, 30 };
    for (int k = 0; k < 5; k++) {
        long long used = backoff_on_429(&b, now);
        CHECK(used == seq[k] * CTU_MIN_MS);
        CHECK(b.until_ms == now + seq[k] * CTU_MIN_MS);
    }
    backoff_reset(&b);
    CHECK(b.until_ms == 0);
    CHECK(backoff_on_429(&b, now) == 5 * CTU_MIN_MS);

    CHECK(ctu_cache_is_newer(200, 100) == 1);
    CHECK(ctu_cache_is_newer(100, 100) == 0);
    CHECK(ctu_cache_is_newer(50, 100) == 0);
    CHECK(ctu_cache_is_newer(0, 0) == 0);

    char tok[256];
    const char *ok = "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"refreshToken\":\"r\",\"expiresAt\":2000}}";
    CHECK(ctu_cred_check(ok, strlen(ok), 1999, tok, sizeof(tok)) == CRED_OK);
    CHECK_STR(tok, "fake-test-token");
    CHECK(ctu_cred_check(ok, strlen(ok), 2000, tok, sizeof(tok)) == CRED_EXPIRED);
    CHECK(tok[0] == 0);
    CHECK(ctu_cred_check(ok, strlen(ok), 5000, tok, sizeof(tok)) == CRED_EXPIRED);
    const char *noexp = "{\"claudeAiOauth\":{\"accessToken\":\"fake\"}}";
    CHECK(ctu_cred_check(noexp, strlen(noexp), 5000, tok, sizeof(tok)) == CRED_OK);
    const char *notok = "{\"claudeAiOauth\":{\"expiresAt\":99999999}}";
    CHECK(ctu_cred_check(notok, strlen(notok), 5000, tok, sizeof(tok)) == CRED_MISSING);
    CHECK(ctu_cred_check(NULL, 0, 5000, tok, sizeof(tok)) == CRED_MISSING);
    CHECK(ctu_cred_check("not json", 8, 5000, tok, sizeof(tok)) == CRED_MISSING);
    CHECK(ctu_cred_check(ok, strlen(ok), 1999, tok, 5) == CRED_MISSING); /* does not fit */
}

/* ---- engine with a fake HTTP function --------------------------------------------------- */

typedef struct {
    int calls;
    int status;
    const char *body;
    char last_token[64];
} fake_http;

static void fake_fetch(void *ctx, const char *token, ctu_http_result *res)
{
    fake_http *f = (fake_http *)ctx;
    f->calls++;
    snprintf(f->last_token, sizeof(f->last_token), "%s", token);
    res->status = f->status;
    if (f->status == 0) {
        snprintf(res->err, sizeof(res->err), "timeout");
        return;
    }
    res->len = strlen(f->body);
    res->body = (char *)malloc(res->len + 1);
    memcpy(res->body, f->body, res->len + 1);
}

static void write_text(const wchar_t *path, const char *text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr;
    WriteFile(h, text, (DWORD)strlen(text), &wr, NULL);
    CloseHandle(h);
}

static void set_mtime(const wchar_t *path, long long ft)
{
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME f;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    SetFileTime(h, NULL, NULL, &f);
    CloseHandle(h);
}

static void test_engine(void)
{
    const char *normal = "{\"five_hour\":{\"utilization\":24.7,\"resets_at\":\"2030-01-01T00:00:00Z\"},"
                         "\"seven_day\":{\"utilization\":82,\"resets_at\":null}}";
    const long long M = CTU_MIN_MS;
    long long t0 = 1900000000000LL;
    char cred[256];
    snprintf(cred, sizeof(cred), "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"expiresAt\":%lld}}",
             t0 + 1000 * M);

    ctu_engine e;
    fake_http f = { 0, 200, normal, "" };
    engine_init(&e, 5);
    _snwprintf(e.cred_path, CTU_PATH_MAX, L"%ls\\cred.json", g_scratch);
    _snwprintf(e.ccjson_path, CTU_PATH_MAX, L"%ls\\claude.json", g_scratch);
    _snwprintf(e.last_path, CTU_PATH_MAX, L"%ls\\last.json", g_scratch);
    DeleteFileW(e.last_path);
    DeleteFileW(e.ccjson_path);
    write_text(e.cred_path, cred);
    e.fetch = fake_fetch;
    e.fetch_ctx = &f;

    /* First step: a request. */
    CHECK(engine_next_due(&e) <= t0);
    CHECK(engine_step(&e, t0, 0) == 1);
    CHECK(f.calls == 1);
    CHECK_STR(f.last_token, "fake-test-token");
    CHECK(e.v.have_data && e.v.source == SRC_LIVE && !e.v.stale && e.v.data.five.pct == 24);
    CHECK(!ctu_view_dimmed(&e.v, t0));

    /* Not due before the interval; "Refresh now" within 5 min sends nothing. */
    engine_step(&e, t0 + 2 * M, 0);
    CHECK(f.calls == 1);
    engine_step(&e, t0 + 2 * M, 1);
    CHECK(f.calls == 1);
    CHECK(strstr(e.v.hint, "next request allowed") != NULL);
    CHECK(engine_next_due(&e) == t0 + 5 * M);

    /* 429 sequence: 5, 10, 20, 30, 30 minutes; Refresh now does not bypass the backoff. */
    f.status = 429;
    f.body = "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"x\"}}";
    long long t = t0 + 5 * M;
    long long seq[] = { 5, 10, 20, 30, 30 };
    for (int k = 0; k < 5; k++) {
        CHECK(engine_next_due(&e) <= t);
        int before = f.calls;
        engine_step(&e, t, 0);
        CHECK(f.calls == before + 1);
        CHECK(e.v.note_kind == NOTE_RATE && e.v.stale);
        CHECK(e.v.bo.until_ms == t + seq[k] * M);
        CHECK(ctu_view_dimmed(&e.v, t));
        engine_step(&e, t + M, 1); /* Refresh now inside the backoff */
        CHECK(f.calls == before + 1);
        CHECK(strstr(e.v.hint, "rate limited until") != NULL);
        CHECK(engine_next_due(&e) == t + seq[k] * M);
        t = e.v.bo.until_ms;
    }
    /* Success resets the backoff. */
    f.status = 200;
    f.body = normal;
    engine_step(&e, t, 0);
    CHECK(e.v.note_kind == NOTE_NONE && !e.v.stale && e.v.bo.next_ms == 5 * M && e.v.data_ms == t);

    /* 401: stale, login note, next try at the next interval. */
    f.status = 401;
    f.body = "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"x\"}}";
    t += 5 * M;
    engine_step(&e, t, 0);
    CHECK(e.v.note_kind == NOTE_LOGIN && e.v.stale && e.v.have_data);
    CHECK(strcmp(e.v.note, CTU_LOGIN_NOTE) == 0);
    CHECK(engine_next_due(&e) == t + 5 * M);

    /* Expired token: no request. */
    int calls = f.calls;
    f.status = 200;
    f.body = normal;
    t += 1000 * M;
    engine_step(&e, t, 0);
    CHECK(f.calls == calls);
    CHECK(e.v.note_kind == NOTE_LOGIN && e.v.stale);
    /* Missing credentials file: no request. */
    DeleteFileW(e.cred_path);
    t += 5 * M;
    engine_step(&e, t, 1);
    CHECK(f.calls == calls);
    CHECK(e.v.note_kind == NOTE_LOGIN);

    /* Transport failure. */
    snprintf(cred, sizeof(cred), "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"expiresAt\":%lld}}",
             t + 100000 * M);
    write_text(e.cred_path, cred);
    f.status = 0;
    t += 5 * M;
    engine_step(&e, t, 0);
    CHECK(f.calls == calls + 1);
    CHECK(e.v.note_kind == NOTE_FAIL && strstr(e.v.note, "timeout") != NULL);
    /* HTTP 500. */
    f.status = 500;
    f.body = "oops";
    t += 5 * M;
    engine_step(&e, t, 0);
    CHECK(e.v.note_kind == NOTE_FAIL && strstr(e.v.note, "HTTP 500") != NULL);

    /* Claude Code's cache newer than our data: used, counted as a fetch, no request. */
    f.status = 200;
    f.body = normal;
    t += 5 * M;
    char cc[512];
    snprintf(cc, sizeof(cc),
             "{\"numStartups\":3,\"cachedUsageUtilization\":{\"fetchedAtMs\":%lld,\"utilization\":"
             "{\"five_hour\":{\"utilization\":61.2,\"resets_at\":null},\"seven_day\":{\"utilization\":90,\"resets_at\":null}}}}",
             t - M);
    write_text(e.ccjson_path, cc);
    calls = f.calls;
    engine_step(&e, t, 0);
    CHECK(f.calls == calls);
    CHECK(e.v.source == SRC_CACHE && e.v.data.five.pct == 61 && e.v.data_ms == t - M && !e.v.stale);
    CHECK(e.v.note_kind == NOTE_NONE);
    CHECK(engine_next_due(&e) == t - M + 5 * M); /* the cache time counts as our fetch */
    /* An older cache (file changed, older fetchedAtMs) is ignored. */
    snprintf(cc, sizeof(cc),
             "{\"cachedUsageUtilization\":{\"fetchedAtMs\":%lld,\"utilization\":{\"five_hour\":{\"utilization\":1,\"resets_at\":null}}}}",
             t - 10 * M);
    write_text(e.ccjson_path, cc);
    set_mtime(e.ccjson_path, 133000000000000000LL);
    CHECK(engine_check_cache(&e, t + M) == 0);
    CHECK(e.v.data.five.pct == 61);
    /* An unchanged file is not read again. */
    CHECK(engine_check_cache(&e, t + 2 * M) == 0);
    /* The key may be absent. */
    write_text(e.ccjson_path, "{\"numStartups\":4}");
    set_mtime(e.ccjson_path, 133000000100000000LL);
    CHECK(engine_check_cache(&e, t + 3 * M) == 0);

    /* last.json: saved after success, loaded by a new engine (no token inside). */
    engine_step(&e, t + 6 * M, 0);
    CHECK(e.v.source == SRC_LIVE);
    size_t len = 0;
    char *saved = ctu_read_file(e.last_path, &len, NULL);
    CHECK(saved != NULL && strstr(saved, "fake-test-token") == NULL && strstr(saved, "\"five_hour\"") != NULL);
    free(saved);
    ctu_engine e2;
    engine_init(&e2, 5);
    wcscpy(e2.last_path, e.last_path);
    CHECK(engine_load_saved(&e2) == 1);
    CHECK(e2.v.have_data && e2.v.source == SRC_SAVED && e2.v.data.five.pct == 24 && e2.v.data.seven.pct == 82);
    CHECK(e2.v.data_ms == t + 6 * M);
    CHECK(e2.v.data.five.reset_ms == utc_ms(2030, 1, 1, 0, 0, 0, 0));
    /* Saved data younger than the interval: no request before it is due. */
    CHECK(engine_next_due(&e2) == t + 11 * M);

    /* Texts. */
    char tip[2048];
    ctu_build_tooltip(&e.v, t + 9 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "5-hour: 24% used, resets ") != NULL);
    CHECK(strstr(tip, "Weekly: 82% used") != NULL);
    CHECK(strstr(tip, "Updated 3 min ago") != NULL);
    ctu_build_tooltip(&e.v, t + 9 * M, 1, tip, sizeof(tip));
    CHECK(strstr(tip, "5-hour: 76% left (24% used)") != NULL);
    char line[1024];
    ctu_build_once_line(&e.v, t + 9 * M, line, sizeof(line));
    CHECK(strstr(line, "5h=24%") != NULL && strstr(line, "W=82%") != NULL && strstr(line, "source=live") != NULL);
    CHECK(strstr(line, "fake-test-token") == NULL);
    ctu_view empty;
    memset(&empty, 0, sizeof(empty));
    ctu_build_tooltip(&empty, t, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "no data yet") != NULL);
    ctu_build_once_line(&empty, t, line, sizeof(line));
    CHECK(strncmp(line, "error:", 6) == 0);

    /* Dimming by age: older than two intervals plus a minute. */
    CHECK(!ctu_view_dimmed(&e.v, t + 6 * M + 11 * M - 1000));
    CHECK(ctu_view_dimmed(&e.v, t + 6 * M + 11 * M + 1000));

    DeleteFileW(e.cred_path);
    DeleteFileW(e.ccjson_path);
    DeleteFileW(e.last_path);
}

/* Writes an invented credentials file with expiresAt and sets its modified time explicitly (no sleep). */
static void write_cred(const wchar_t *path, long long expires_at, long long ft)
{
    char cred[256];
    snprintf(cred, sizeof(cred), "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"expiresAt\":%lld}}",
             expires_at);
    write_text(path, cred);
    set_mtime(path, ft);
}

static void login_engine(ctu_engine *e, fake_http *f, const wchar_t *name)
{
    engine_init(e, 30); /* a long interval: a retry far inside it shows the new rule */
    _snwprintf(e->cred_path, CTU_PATH_MAX, L"%ls\\%ls", g_scratch, name);
    e->fetch = fake_fetch;
    e->fetch_ctx = f;
}

/* Login retry: while NOTE_LOGIN, a change of the credentials file makes the next step try at once. */
static void test_login_retry(void)
{
    const char *normal = "{\"five_hour\":{\"utilization\":24.7,\"resets_at\":\"2030-01-01T00:00:00Z\"},"
                         "\"seven_day\":{\"utilization\":82,\"resets_at\":null}}";
    const long long M = CTU_MIN_MS, S = 1000;
    const long long T = 1900000000000LL;
    const long long FT1 = 133100000000000000LL, FT2 = 133100000100000000LL, FT3 = 133100000200000000LL;
    ctu_engine e;

    /* Expired token, then the file is rewritten with a valid token. */
    {
        fake_http f = { 0, 200, normal, "" };
        login_engine(&e, &f, L"cred_lr1.json");
        write_cred(e.cred_path, T - M, FT1);
        CHECK(engine_step(&e, T, 0) == 1);
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_step(&e, T + 10 * S, 0) == 0); /* file unchanged */
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_next_due(&e) == T + 30 * M); /* without a change: the interval, as before */
        write_cred(e.cred_path, T + 1000 * M, FT2);
        CHECK(engine_step(&e, T + 20 * S, 0) == 1);
        CHECK(f.calls == 1);
        CHECK(e.v.note_kind == NOTE_NONE && !e.v.stale && e.v.have_data && e.v.source == SRC_LIVE);
        CHECK(e.v.data_ms == T + 20 * S && e.v.data.five.pct == 24);
        DeleteFileW(e.cred_path);
    }

    /* Missing file, then the file appears with a valid token. */
    {
        fake_http f = { 0, 200, normal, "" };
        login_engine(&e, &f, L"cred_lr2.json");
        DeleteFileW(e.cred_path);
        CHECK(engine_step(&e, T, 0) == 1);
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_step(&e, T + 10 * S, 0) == 0);
        CHECK(f.calls == 0);
        write_cred(e.cred_path, T + 1000 * M, FT1);
        CHECK(engine_step(&e, T + 20 * S, 0) == 1);
        CHECK(f.calls == 1);
        CHECK(e.v.note_kind == NOTE_NONE && !e.v.stale && e.v.have_data && e.v.source == SRC_LIVE);
        DeleteFileW(e.cred_path);
    }

    /* File changed, but the token is still expired: no request, the note stays, no loop. */
    {
        fake_http f = { 0, 200, normal, "" };
        login_engine(&e, &f, L"cred_lr3.json");
        write_cred(e.cred_path, T - M, FT1);
        engine_step(&e, T, 0);
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        write_cred(e.cred_path, T - 2 * M, FT2);
        engine_step(&e, T + 10 * S, 0); /* the retry reads the file */
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(e.v.last_attempt_ms == T + 10 * S);
        CHECK(engine_step(&e, T + 20 * S, 0) == 0); /* no further change: no retry */
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN && e.v.last_attempt_ms == T + 10 * S);
        CHECK(engine_next_due(&e) == T + 10 * S + 30 * M);
        DeleteFileW(e.cred_path);
    }

    /* HTTP 401 at T, the file changes at T + 10 s: the 5 min minimum between requests still applies. */
    {
        fake_http f = { 0, 401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"x\"}}",
                        "" };
        login_engine(&e, &f, L"cred_lr4.json");
        write_cred(e.cred_path, T + 1000 * M, FT1);
        engine_step(&e, T, 0);
        CHECK(f.calls == 1 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_next_due(&e) == T + 30 * M);
        f.status = 200;
        f.body = normal;
        write_cred(e.cred_path, T + 2000 * M, FT2);
        CHECK(engine_step(&e, T + 10 * S, 0) == 0);
        CHECK(f.calls == 1 && e.v.hint[0] == 0);
        CHECK(engine_next_due(&e) == T + 5 * M);
        CHECK(engine_step(&e, T + 2 * M, 0) == 0);
        CHECK(engine_step(&e, T + 5 * M - 1, 0) == 0);
        CHECK(f.calls == 1 && e.v.hint[0] == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_next_due(&e) == T + 5 * M);
        CHECK(engine_step(&e, T + 5 * M, 0) == 1);
        CHECK(f.calls == 2 && e.v.note_kind == NOTE_NONE && e.v.have_data && e.v.data_ms == T + 5 * M);
        DeleteFileW(e.cred_path);
    }

    /* Backoff active and a login retry due: no request before bo.until_ms. The backoff is set directly:
     * the normal step order gives NOTE_LOGIN only after the backoff ended. */
    {
        fake_http f = { 0, 200, normal, "" };
        login_engine(&e, &f, L"cred_lr5.json");
        write_cred(e.cred_path, T - M, FT1);
        engine_step(&e, T, 0);
        CHECK(f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        e.v.bo.until_ms = T + 20 * M;
        write_cred(e.cred_path, T + 1000 * M, FT3);
        CHECK(engine_step(&e, T + 10 * S, 0) == 0);
        CHECK(f.calls == 0 && e.v.hint[0] == 0);
        CHECK(engine_next_due(&e) == T + 20 * M);
        CHECK(engine_step(&e, T + 20 * M - 1, 0) == 0);
        CHECK(f.calls == 0);
        CHECK(engine_step(&e, T + 20 * M, 0) == 1);
        CHECK(f.calls == 1 && e.v.note_kind == NOTE_NONE && e.v.have_data);
        DeleteFileW(e.cred_path);
    }
}

/* A renew hook that counts its calls and, when write_expires is set, rewrites the credentials file. */
typedef struct {
    int calls;
    const wchar_t *path;
    long long write_expires; /* 0 = change nothing */
    long long write_ft;
} fake_renew;

static void fake_renew_fn(void *ctx)
{
    fake_renew *r = (fake_renew *)ctx;
    r->calls++;
    if (r->write_expires) write_cred(r->path, r->write_expires, r->write_ft);
}

static void renew_engine(ctu_engine *e, fake_http *f, fake_renew *r, const wchar_t *name, int interval)
{
    engine_init(e, interval);
    _snwprintf(e->cred_path, CTU_PATH_MAX, L"%ls\\%ls", g_scratch, name);
    e->fetch = fake_fetch;
    e->fetch_ctx = f;
    e->renew = fake_renew_fn;
    e->renew_ctx = r;
    r->path = e->cred_path;
}

/* Login renewal through the hook: only for an expired token in an existing file, at most once per 30 min. */
static void test_renew(void)
{
    const char *normal = "{\"five_hour\":{\"utilization\":24.7,\"resets_at\":\"2030-01-01T00:00:00Z\"},"
                         "\"seven_day\":{\"utilization\":82,\"resets_at\":null}}";
    const long long M = CTU_MIN_MS, S = 1000;
    const long long T = 1900000000000LL;
    const long long FT1 = 133200000000000000LL, FT2 = 133200000100000000LL;
    ctu_engine e;

    /* Expired token, the hook writes a valid one: one hook call, one request, live data, no note. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, T + 1000 * M, FT2 };
        renew_engine(&e, &f, &r, L"cred_rn1.json", 30);
        write_cred(e.cred_path, T - M, FT1);
        CHECK(engine_step(&e, T, 0) == 1);
        CHECK(r.calls == 1 && f.calls == 1);
        CHECK(e.v.note_kind == NOTE_NONE && !e.v.stale && e.v.have_data && e.v.source == SRC_LIVE);
        CHECK(e.v.data_ms == T && e.v.data.five.pct == 24 && e.last_renew_ms == T);
        DeleteFileW(e.cred_path);
    }

    /* Expired token, the hook changes nothing: NOTE_LOGIN; no second call 10 min later, one 31 min later. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, 0, 0 };
        renew_engine(&e, &f, &r, L"cred_rn2.json", 5);
        write_cred(e.cred_path, T - M, FT1);
        CHECK(engine_step(&e, T, 0) == 1);
        CHECK(r.calls == 1 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN && e.v.stale);
        CHECK(engine_step(&e, T + 10 * M, 0) == 1);
        CHECK(e.v.last_attempt_ms == T + 10 * M); /* the step reached the credential check */
        CHECK(r.calls == 1 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_step(&e, T + 31 * M, 0) == 1);
        CHECK(r.calls == 2 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        DeleteFileW(e.cred_path);
    }

    /* After a failed renewal, a later change of the file (valid token): a request without a new hook call. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, 0, 0 };
        renew_engine(&e, &f, &r, L"cred_rn3.json", 30);
        write_cred(e.cred_path, T - M, FT1);
        engine_step(&e, T, 0);
        CHECK(r.calls == 1 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        CHECK(engine_step(&e, T + 10 * S, 0) == 0); /* file unchanged: no retry */
        write_cred(e.cred_path, T + 1000 * M, FT2);
        CHECK(engine_step(&e, T + 20 * S, 0) == 1);
        CHECK(r.calls == 1 && f.calls == 1);
        CHECK(e.v.note_kind == NOTE_NONE && e.v.have_data && e.v.data_ms == T + 20 * S);
        DeleteFileW(e.cred_path);
    }

    /* "Refresh now" two minutes after a failed renewal: the hook is called again. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, 0, 0 };
        renew_engine(&e, &f, &r, L"cred_rn4.json", 30);
        write_cred(e.cred_path, T - M, FT1);
        engine_step(&e, T, 0);
        CHECK(r.calls == 1);
        CHECK(engine_step(&e, T + 2 * M, 1) == 1);
        CHECK(r.calls == 2 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        DeleteFileW(e.cred_path);
    }

    /* Missing or unreadable file, HTTP 401: no hook call. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, T + 1000 * M, FT2 };
        renew_engine(&e, &f, &r, L"cred_rn5.json", 5);
        DeleteFileW(e.cred_path);
        engine_step(&e, T, 1);
        CHECK(r.calls == 0 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        write_text(e.cred_path, "not json");
        engine_step(&e, T + 40 * M, 1);
        CHECK(r.calls == 0 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        write_cred(e.cred_path, T + 1000 * M, FT1);
        f.status = 401;
        f.body = "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"x\"}}";
        engine_step(&e, T + 80 * M, 1);
        CHECK(f.calls == 1 && r.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        DeleteFileW(e.cred_path);
    }

    /* Hook NULL: as before (NOTE_LOGIN, no request). */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, T + 1000 * M, FT2 };
        renew_engine(&e, &f, &r, L"cred_rn6.json", 5);
        e.renew = NULL;
        write_cred(e.cred_path, T - M, FT1);
        CHECK(engine_step(&e, T, 1) == 1);
        CHECK(r.calls == 0 && f.calls == 0 && e.v.note_kind == NOTE_LOGIN);
        DeleteFileW(e.cred_path);
    }

    /* Sample mode (skip_credentials): no hook call, the request is sent. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, T + 1000 * M, FT2 };
        renew_engine(&e, &f, &r, L"cred_rn7.json", 5);
        e.skip_credentials = 1;
        e.live_source = SRC_SAMPLE;
        write_cred(e.cred_path, T - M, FT1);
        engine_step(&e, T, 1);
        CHECK(r.calls == 0 && f.calls == 1 && e.v.note_kind == NOTE_NONE && e.v.source == SRC_SAMPLE);
        DeleteFileW(e.cred_path);
    }

    /* Backoff active: no hook call. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, T + 1000 * M, FT2 };
        renew_engine(&e, &f, &r, L"cred_rn8.json", 5);
        write_cred(e.cred_path, T - M, FT1);
        e.v.bo.until_ms = T + 20 * M;
        engine_step(&e, T, 1);
        CHECK(r.calls == 0 && f.calls == 0 && strstr(e.v.hint, "rate limited until") != NULL);
        engine_step(&e, T + 10 * M, 0);
        CHECK(r.calls == 0 && f.calls == 0);
        DeleteFileW(e.cred_path);
    }

    /* Inside the 5 minute minimum: no hook call; at the next due step the hook runs. */
    {
        fake_http f = { 0, 200, normal, "" };
        fake_renew r = { 0, NULL, 0, 0 };
        renew_engine(&e, &f, &r, L"cred_rn9.json", 5);
        write_cred(e.cred_path, T + M, FT1); /* valid at T, expired from T + 1 min */
        engine_step(&e, T, 0);
        CHECK(f.calls == 1 && r.calls == 0 && e.v.note_kind == NOTE_NONE);
        engine_step(&e, T + 2 * M, 1);
        CHECK(r.calls == 0 && f.calls == 1 && strstr(e.v.hint, "next request allowed") != NULL);
        engine_step(&e, T + 5 * M, 0);
        CHECK(r.calls == 1 && f.calls == 1 && e.v.note_kind == NOTE_LOGIN);
        DeleteFileW(e.cred_path);
    }
}

/* The environment filter of the real renew hook. */
static void test_env_filter(void)
{
    static const wchar_t in[] = L"=C:=C:\\x\0PATH=C:\\Windows\0CLAUDE_X=1\0claudecode=1\0ANTHROPIC_BASE_URL=http://x\0"
                                L"USERPROFILE=C:\\Users\\u\0MYCLAUDE=2\0Claude=3\0";
    static const wchar_t want[] = L"=C:=C:\\x\0PATH=C:\\Windows\0USERPROFILE=C:\\Users\\u\0MYCLAUDE=2\0";
    wchar_t out[256];
    CHECK(ctu_env_block_len(in) == sizeof(in) / sizeof(in[0]));
    size_t n = ctu_env_filter(in, out, 256);
    CHECK(n == sizeof(want) / sizeof(want[0]));
    CHECK(n == sizeof(want) / sizeof(want[0]) && memcmp(out, want, sizeof(want)) == 0);
    CHECK(n >= 2 && out[n - 1] == 0 && out[n - 2] == 0);
    CHECK(ctu_env_block_len(out) == n);

    /* Everything removed: an empty block (two NULs) is valid. */
    static const wchar_t all[] = L"CLAUDE_A=1\0ANTHROPIC_B=2\0";
    for (int i = 0; i < 8; i++) out[i] = L'#';
    n = ctu_env_filter(all, out, 256);
    CHECK(n == 2 && out[0] == 0 && out[1] == 0);
    /* An empty input block. */
    static const wchar_t none[] = L"";
    for (int i = 0; i < 8; i++) out[i] = L'#';
    CHECK(ctu_env_block_len(none) == 1);
    n = ctu_env_filter(none, out, 256);
    CHECK(n == 2 && out[0] == 0 && out[1] == 0);
    /* Too small an output buffer. */
    CHECK(ctu_env_filter(in, out, ctu_env_block_len(in)) == 0);
}

/* The real hook's inner function with hidden system programs (no window): exit, environment, working folder,
 * time-out, stop event and the end of the process tree. Never claude.exe. */
static void test_renew_run(void)
{
    wchar_t sys[MAX_PATH], cmd[1024], path[MAX_PATH + 64];
    GetSystemDirectoryW(sys, MAX_PATH);

    _snwprintf(cmd, 1024, L"\"%ls\\cmd.exe\" /d /c exit 0", sys);
    CHECK(ctu_renew_run(cmd, g_scratch, NULL, 10000) == RENEW_EXITED);
    CHECK(ctu_renew_run(L"\"C:\\nonexistent-ctu\\nope.exe\" mcp list", g_scratch, NULL, 10000) == RENEW_NOT_STARTED);
    CHECK(ctu_renew_run(L"", g_scratch, NULL, 10000) == RENEW_NOT_STARTED);

    /* Environment without CLAUDE* / ANTHROPIC*; the working folder is the given one. */
    SetEnvironmentVariableW(L"CLAUDE_CTU_TEST", L"x1");
    SetEnvironmentVariableW(L"anthropic_ctu_test", L"x2");
    SetEnvironmentVariableW(L"MYCLAUDE_KEEP", L"x3");
    _snwprintf(path, MAX_PATH + 64, L"%ls\\renew_env.txt", g_scratch);
    DeleteFileW(path);
    _snwprintf(cmd, 1024, L"\"%ls\\cmd.exe\" /d /c \"set > renew_env.txt & cd >> renew_env.txt\"", sys);
    CHECK(ctu_renew_run(cmd, g_scratch, NULL, 10000) == RENEW_EXITED);
    SetEnvironmentVariableW(L"CLAUDE_CTU_TEST", NULL);
    SetEnvironmentVariableW(L"anthropic_ctu_test", NULL);
    SetEnvironmentVariableW(L"MYCLAUDE_KEEP", NULL);
    size_t len = 0;
    char *txt = ctu_read_file(path, &len, NULL);
    char scratch8[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, g_scratch, -1, scratch8, sizeof(scratch8), NULL, NULL);
    CHECK(txt != NULL);
    if (txt) {
        CHECK(strstr(txt, "CLAUDE_CTU_TEST=") == NULL);
        CHECK(strstr(txt, "anthropic_ctu_test=") == NULL);
        CHECK(strstr(txt, "MYCLAUDE_KEEP=x3") != NULL);
        CHECK(strstr(txt, "SystemRoot=") != NULL);
        CHECK(strstr(txt, scratch8) != NULL);
    }
    free(txt);
    DeleteFileW(path);

    /* Time-out: a short value ends the job. */
    _snwprintf(cmd, 1024, L"\"%ls\\PING.EXE\" -n 30 127.0.0.1", sys);
    ULONGLONG t0 = GetTickCount64();
    CHECK(ctu_renew_run(cmd, g_scratch, NULL, 300) == RENEW_TIMEOUT);
    CHECK(GetTickCount64() - t0 < 3000);

    /* Stop event set: returns at once. */
    HANDLE stop = CreateEventW(NULL, TRUE, TRUE, NULL);
    t0 = GetTickCount64();
    CHECK(ctu_renew_run(cmd, g_scratch, stop, 60000) == RENEW_STOPPED);
    CHECK(GetTickCount64() - t0 < 1500);
    CloseHandle(stop);

    /* The tree: the direct child exits at once, its child (started with start /b) would write a marker after
     * about 1 s; closing the job ends it, so the marker never appears. */
    _snwprintf(path, MAX_PATH + 64, L"%ls\\renew_tree.txt", g_scratch);
    DeleteFileW(path);
    _snwprintf(cmd, 1024,
               L"\"%ls\\cmd.exe\" /d /c start \"\" /b cmd.exe /d /c \"ping -n 2 127.0.0.1 >NUL & echo x>renew_tree.txt\"",
               sys);
    CHECK(ctu_renew_run(cmd, g_scratch, NULL, 10000) == RENEW_EXITED);
    Sleep(2500);
    CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(path);
}

/* Fable weekly limit from the "limits" list (Brief 2). */
static void test_fable(void)
{
    usage_t u;
    char err[160], et[64];
    ctu_bar bar;

    /* The live answer form of 2026-11-12: top-level keys plus limits[]. */
    CHECK(parse_sample("fable.json", &u, err, et) == 1);
    CHECK(u.five.present && u.five.pct == 3 && u.seven.present && u.seven.pct == 20);
    CHECK_STR(u.five.key, "five_hour");
    CHECK_STR(u.seven.key, "seven_day");
    CHECK(u.five.reset_ms == utc_ms(2026, 11, 12, 7, 19, 59, 951));
    CHECK(u.nextra == 1); /* session and weekly_all add no row; the null top-level keys add none */
    CHECK_STR(u.extra[0].key, "weekly_scoped:Fable");
    CHECK_STR(u.extra[0].name, "Weekly Fable");
    CHECK(u.extra[0].present && u.extra[0].pct == 27);
    CHECK(u.extra[0].reset_ms == utc_ms(2026, 11, 17, 16, 0, 0, 952));
    CHECK_STR(u.extra[0].reset_iso, "2026-11-17T16:00:00.952084+00:00");
    CHECK(ctu_is_fable(&u.extra[0]) && ctu_is_model_scoped(&u.extra[0]));
    CHECK(ctu_find_fable(&u) == &u.extra[0]);
    CHECK(ctu_bar_values(&u, 0, &bar) == 3);
    CHECK_STR(bar.label[0], "5h");
    CHECK_STR(bar.label[1], "W");
    CHECK_STR(bar.label[2], "F");
    CHECK_STR(bar.text[0], "3%");
    CHECK_STR(bar.text[1], "20%");
    CHECK_STR(bar.text[2], "27%");
    CHECK(bar.level[0] == 0 && bar.level[1] == 0 && bar.level[2] == 0);
    ctu_bar_values(&u, 1, &bar); /* show remaining */
    CHECK_STR(bar.text[0], "97%");
    CHECK_STR(bar.text[1], "80%");
    CHECK_STR(bar.text[2], "73%");
    CHECK(bar.level[2] == 0); /* the colour follows the USED value */

    /* Orange and red: same thresholds as the other two values. */
    CHECK(parse_sample("fable_orange.json", &u, err, et) == 1);
    ctu_bar_values(&u, 0, &bar);
    CHECK(bar.n == 3 && bar.level[0] == 0 && bar.level[1] == 1 && bar.level[2] == 1);
    CHECK_STR(bar.text[2], "86%");
    ctu_bar_values(&u, 1, &bar);
    CHECK_STR(bar.text[2], "14%");
    CHECK(bar.level[2] == 1);
    CHECK(parse_sample("fable_red.json", &u, err, et) == 1);
    ctu_bar_values(&u, 0, &bar);
    CHECK(bar.n == 3 && bar.level[0] == 2 && bar.level[1] == 1 && bar.level[2] == 2);
    CHECK_STR(bar.text[0], "97%");
    CHECK_STR(bar.text[2], "96%");

    /* Two values when no Fable window is present (the earlier forms). */
    CHECK(parse_sample("normal.json", &u, err, et) == 1);
    CHECK(ctu_find_fable(&u) == NULL);
    CHECK(ctu_bar_values(&u, 0, &bar) == 2);
    CHECK_STR(bar.text[0], "24%");
    CHECK_STR(bar.text[1], "82%");
    CHECK(bar.text[2][0] == 0 && bar.label[2][0] == 0);
    CHECK(ctu_bar_values(NULL, 0, &bar) == 2); /* no data yet */
    CHECK_STR(bar.text[0], "--");
    CHECK_STR(bar.text[1], "--");
    /* limits.json (limits only): fallback for five and seven still works, and Fable shows. */
    CHECK(parse_sample("limits.json", &u, err, et) == 1);
    CHECK(u.five.pct == 51 && u.seven.pct == 6 && u.nextra == 1);
    CHECK(ctu_find_fable(&u) != NULL && ctu_find_fable(&u)->pct == 12);

    /* Surface scope, scope without a name, a lower-case duplicate Fable row; top-level keys win. */
    CHECK(parse_sample("scoped_surface.json", &u, err, et) == 1);
    CHECK(u.five.pct == 10 && u.seven.pct == 30);
    CHECK(u.nextra == 4);
    CHECK_STR(u.extra[0].key, "weekly_scoped:fable");   /* Fable first: directly after "Weekly" */
    CHECK(u.extra[0].pct == 27);                        /* the second Fable row (99) is not added */
    CHECK_STR(u.extra[1].name, "Weekly Opus");
    CHECK_STR(u.extra[2].name, "Weekly (Claude Code)");
    CHECK_STR(u.extra[2].key, "weekly_scoped_other:Claude Code");
    CHECK(u.extra[2].pct == 44 && !ctu_is_model_scoped(&u.extra[2]) && !ctu_is_fable(&u.extra[2]));
    CHECK_STR(u.extra[3].name, "Weekly (scoped)");
    CHECK(u.extra[3].pct == 12 && !ctu_is_model_scoped(&u.extra[3]));
    CHECK(ctu_bar_values(&u, 0, &bar) == 3);
    CHECK_STR(bar.text[2], "27%");

    /* A scoped row without a model name never reaches the taskbar. */
    jdoc d = { 0 };
    const char *only_surface = "{\"five_hour\":{\"utilization\":1,\"resets_at\":null},\"limits\":[{\"kind\":\"weekly_scoped\","
                               "\"percent\":70,\"resets_at\":null,\"scope\":{\"model\":null,\"surface\":\"cowork\"}}]}";
    CHECK(usage_parse(parse(&d, only_surface), &u, err, sizeof(err), et, sizeof(et)) == 1);
    CHECK(u.nextra == 1);
    CHECK_STR(u.extra[0].name, "Weekly (cowork)");
    CHECK(ctu_find_fable(&u) == NULL && ctu_bar_values(&u, 0, &bar) == 2);
    json_free(&d);
    /* Another model: tooltip only, ordered before other extras but after Fable. */
    const char *two_models = "{\"seven_day\":{\"utilization\":5,\"resets_at\":null},\"extra_usage\":{\"utilization\":9,\"resets_at\":null},"
                             "\"limits\":[{\"kind\":\"weekly_scoped\",\"percent\":33,\"scope\":{\"model\":{\"display_name\":\"Opus\"}}},"
                             "{\"kind\":\"weekly_scoped\",\"percent\":44,\"scope\":{\"model\":{\"display_name\":\"FABLE\"}}}]}";
    CHECK(usage_parse(parse(&d, two_models), &u, err, sizeof(err), et, sizeof(et)) == 1);
    CHECK(u.nextra == 3);
    CHECK_STR(u.extra[0].name, "Weekly FABLE");
    CHECK_STR(u.extra[1].name, "Weekly Opus");
    CHECK_STR(u.extra[2].name, "Extra usage");
    CHECK(ctu_find_fable(&u) == &u.extra[0] && ctu_find_fable(&u)->pct == 44);
    json_free(&d);
    /* A Fable row with a null percent: no number, not shown. */
    const char *null_pct = "{\"five_hour\":{\"utilization\":1,\"resets_at\":null},\"limits\":[{\"kind\":\"weekly_scoped\","
                           "\"percent\":null,\"scope\":{\"model\":{\"display_name\":\"Fable\"}}}]}";
    CHECK(usage_parse(parse(&d, null_pct), &u, err, sizeof(err), et, sizeof(et)) == 1);
    CHECK(u.nextra == 0 && ctu_find_fable(&u) == NULL);
    json_free(&d);

    /* Layout: the third value adds separator gaps, the label, the gap and the number slot. */
    ctu_metrics m = { 4, 16, 6, 4, 8, 1, 20 };
    int wl[3] = { 10, 7, 6 }, wn[3] = { 18, 25, 20 };
    ctu_layout lay;
    CHECK(ctu_bar_layout(&m, 2, wl, wn, &lay) == 117);
    CHECK(lay.n == 2 && lay.icon_x == 4 && lay.label_x[0] == 26 && lay.num_x[0] == 40);
    CHECK(lay.sep_x[0] == 68 && lay.label_x[1] == 77 && lay.num_x[1] == 88);
    CHECK(lay.width == 4 + 16 + 6 + 10 + 4 + 20 + 8 + 1 + 8 + 7 + 4 + 25 + 4); /* the formula of the two-value form */
    CHECK(ctu_bar_layout(&m, 3, wl, wn, &lay) == 164);
    CHECK(lay.sep_x[0] == 68 && lay.sep_x[1] == 121 && lay.label_x[2] == 130 && lay.num_x[2] == 140);
}

/* The Claude spark mask and its scaler. */
static void test_logo(void)
{
    int n = ctu_logo_n();
    CHECK(n == CTU_LOGO_N && n >= 64 && n <= 256);
    const unsigned char *m = ctu_logo_mask();
    int nz = 0, full = 0;
    for (int i = 0; i < n * n; i++) {
        if (m[i]) nz++;
        if (m[i] == 255) full++;
    }
    CHECK(nz > n * n / 10 && nz < n * n * 9 / 10); /* not empty, not a filled square */
    CHECK(full > n * n / 20);
    /* The corners are empty (a spark, not the tile), the centre is full. */
    CHECK(m[0] == 0 && m[n - 1] == 0 && m[(n - 1) * n] == 0 && m[n * n - 1] == 0);
    CHECK(m[(n / 2) * n + n / 2] == 255);
    unsigned char r, g, b;
    ctu_logo_color(&r, &g, &b);
    CHECK(r > 0xC0 && g > 0x60 && g < 0x90 && b > 0x40 && b < 0x70); /* Claude orange */
    int sizes[] = { 16, 20, 24 };
    for (int k = 0; k < 3; k++) {
        int s = sizes[k];
        unsigned char out[24 * 24];
        memset(out, 0xEE, sizeof(out));
        int cnt = ctu_logo_scale(s, out);
        CHECK(cnt > s * s / 10 && cnt < s * s);
        CHECK(out[(s / 2) * s + s / 2] > 200); /* the centre stays solid */
        CHECK(out[0] == 0 && out[s * s - 1] == 0);
        /* Area average: the mean coverage stays the mean of the source (within 1 %). */
        double src = 0, dst = 0;
        for (int i = 0; i < n * n; i++) src += m[i];
        for (int i = 0; i < s * s; i++) dst += out[i];
        src /= (double)n * n;
        dst /= (double)s * s;
        CHECK(dst > src - 2.6 && dst < src + 2.6);
    }
    unsigned char one[1];
    CHECK(ctu_logo_scale(0, one) == -1);
    CHECK(ctu_logo_scale(257, one) == -1);
}

/* last.json keeps the Fable window; the form written by the installed version still loads. */
static void test_fable_saved(void)
{
    const long long M = CTU_MIN_MS;
    long long t0 = 1900000000000LL;
    size_t blen = 0;
    char *body = read_sample("fable.json", &blen);
    CHECK(body != NULL);
    if (!body) return;
    char cred[256];
    snprintf(cred, sizeof(cred), "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"expiresAt\":%lld}}",
             t0 + 1000 * M);
    ctu_engine e;
    fake_http f = { 0, 200, body, "" };
    engine_init(&e, 5);
    _snwprintf(e.cred_path, CTU_PATH_MAX, L"%ls\\cred_f.json", g_scratch);
    _snwprintf(e.ccjson_path, CTU_PATH_MAX, L"%ls\\claude_f.json", g_scratch);
    _snwprintf(e.last_path, CTU_PATH_MAX, L"%ls\\last_f.json", g_scratch);
    DeleteFileW(e.last_path);
    DeleteFileW(e.ccjson_path);
    write_text(e.cred_path, cred);
    e.fetch = fake_fetch;
    e.fetch_ctx = &f;
    CHECK(engine_step(&e, t0, 0) == 1);
    CHECK(e.v.have_data && ctu_find_fable(&e.v.data) != NULL);

    size_t len = 0;
    char *saved = ctu_read_file(e.last_path, &len, NULL);
    CHECK(saved != NULL && strstr(saved, "\"weekly_scoped:Fable\"") != NULL && strstr(saved, "\"Weekly Fable\"") != NULL);
    CHECK(saved != NULL && strstr(saved, "fake-test-token") == NULL);
    free(saved);

    ctu_engine e2;
    engine_init(&e2, 5);
    wcscpy(e2.last_path, e.last_path);
    CHECK(engine_load_saved(&e2) == 1);
    CHECK(e2.v.data.five.pct == 3 && e2.v.data.seven.pct == 20 && e2.v.data.nextra == 1);
    const uwin *fw = ctu_find_fable(&e2.v.data);
    CHECK(fw != NULL);
    if (fw) {
        CHECK(fw->pct == 27);
        CHECK_STR(fw->name, "Weekly Fable");
        CHECK_STR(fw->key, "weekly_scoped:Fable");
        CHECK(fw->reset_ms == utc_ms(2026, 11, 17, 16, 0, 0, 952));
    }
    ctu_bar bar;
    CHECK(ctu_bar_values(&e2.v.data, 0, &bar) == 3);
    CHECK_STR(bar.text[2], "27%");

    /* Tooltip: "Weekly Fable" once, directly after "Weekly". */
    char tip[2048];
    ctu_build_tooltip(&e2.v, t0 + 2 * M, 0, tip, sizeof(tip));
    const char *wk = strstr(tip, "Weekly: 20% used");
    const char *fb = strstr(tip, "Weekly Fable: 27% used, resets ");
    CHECK(wk != NULL && fb != NULL);
    if (wk && fb) {
        const char *eol = strstr(wk, "\r\n");
        CHECK(eol != NULL && eol + 2 == fb);
        CHECK(strstr(fb + 1, "Weekly Fable") == NULL);
    }
    ctu_build_tooltip(&e2.v, t0 + 2 * M, 1, tip, sizeof(tip));
    CHECK(strstr(tip, "Weekly Fable: 73% left (27% used)") != NULL);

    /* --once line has the Fable value. */
    char line[1024];
    ctu_build_once_line(&e2.v, t0 + 2 * M, line, sizeof(line));
    CHECK(strstr(line, "5h=3%") != NULL && strstr(line, "W=20%") != NULL);
    CHECK(strstr(line, "Weekly Fable=27% resets_at=2026-11-17T16:00:00.952084+00:00") != NULL);
    CHECK(strstr(line, "source=saved") != NULL);

    /* A last.json of the installed version (no Fable window). */
    write_text(e.last_path, "{\"fetchedAtMs\":1794450219575,\"source\":\"live\",\"utilization\":{\"five_hour\":{\"utilization\":2,"
                            "\"resets_at\":\"2026-11-12T07:19:59.766428+00:00\",\"name\":\"5-hour\"},\"seven_day\":{\"utilization\":20,"
                            "\"resets_at\":\"2026-11-17T16:00:00.766478+00:00\",\"name\":\"Weekly\"}}}\r\n");
    ctu_engine e3;
    engine_init(&e3, 5);
    wcscpy(e3.last_path, e.last_path);
    CHECK(engine_load_saved(&e3) == 1);
    CHECK(e3.v.data.five.pct == 2 && e3.v.data.seven.pct == 20 && e3.v.data.nextra == 0);
    CHECK(e3.v.data_ms == 1794450219575LL);
    CHECK(ctu_find_fable(&e3.v.data) == NULL);
    CHECK(ctu_bar_values(&e3.v.data, 0, &bar) == 2);
    ctu_build_tooltip(&e3.v, 1794450219575LL + M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Fable") == NULL && strstr(tip, "Weekly: 20% used") != NULL);

    DeleteFileW(e.cred_path);
    DeleteFileW(e.ccjson_path);
    DeleteFileW(e.last_path);
    free(body);
}

/* ---- the VS Code usage extension's records (invented sample files) ------------------------- */

static void ext_rec(char *out, size_t cap, long long obs, const char *pt, double frac, long long reset, int schema,
                    const char *prov)
{
    size_t n = strlen(out);
    snprintf(out + n, cap - n,
             "%s{\"schemaVersion\":%d,\"provider\":\"%s\",\"accountFingerprint\":\"fp-test-%lld\",\"observedAt\":%lld,"
             "\"periodType\":\"%s\",\"usedFraction\":%.6g,\"remainingFraction\":%.6g,\"resetAt\":%lld,"
             "\"source\":\"claude-official-api\",\"windowId\":\"win_t\",\"confidence\":\"high\",\"flags\":[]}",
             strchr(out, '{') && out[n - 1] == '}' ? ",\n" : "", schema, prov, obs % 97, obs, pt, frac, 1.0 - frac, reset);
}

static void ext_begin(char *out, size_t cap)
{
    snprintf(out, cap, "{\"schemaVersion\":2,\"fingerprintAlgorithm\":\"hmac-sha256-v1\",\"observations\":[\n");
}

static void ext_end(char *out, size_t cap)
{
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "\n]}\n");
}

/* One fetch: three records with the same observedAt (scoped < 0 = no scoped record). */
static void ext_set(char *out, size_t cap, long long obs, double five, double seven, double scoped, long long reset5,
                    long long reset7)
{
    ext_rec(out, cap, obs, "five-hour", five, reset5, 2, "claude");
    if (scoped >= 0) ext_rec(out, cap, obs, "provider-scoped-seven-day", scoped, reset7, 2, "claude");
    ext_rec(out, cap, obs, "seven-day", seven, reset7, 2, "claude");
}

static void ext_dir_path(wchar_t *out, const char *name)
{
    _snwprintf(out, CTU_PATH_MAX, L"%ls\\%hs", g_scratch, name);
    out[CTU_PATH_MAX - 1] = 0;
    CreateDirectoryW(out, NULL);
}

static void ext_write(const wchar_t *dir, const wchar_t *file, const char *text, long long ft)
{
    wchar_t p[CTU_PATH_MAX + 64];
    _snwprintf(p, CTU_PATH_MAX + 64, L"%ls\\%ls", dir, file);
    p[CTU_PATH_MAX + 63] = 0;
    if (!text) {
        DeleteFileW(p);
        return;
    }
    write_text(p, text);
    if (ft) set_mtime(p, ft);
}

static int build_text(const char *s, const char *label, long long now, usage_t *u, long long *obs)
{
    return ctu_ext_build(s, strlen(s), label, now, u, obs);
}

static void test_ext_scan(void)
{
    ctu_ext_span sp[16];
    /* Cut first object, braces and brackets inside strings, escaped quotes and backslashes. */
    const char *a = "ew\":\"x}\",\"q\":1},{\"a\":\"}{][\"},{\"b\":\"q\\\"}\\\\\",\"c\":[{\"d\":2}]}\r\n]}\r\n";
    int n = ctu_ext_scan(a, strlen(a), sp, 16);
    CHECK(n == 3); /* {"b":...}, its nested array element {"d":2}, {"a":...}; not the cut object */
    if (n == 3) {
        CHECK(strncmp(a + sp[0].off, "{\"d\":2}", sp[0].len) == 0 && sp[0].len == 7);
        CHECK(strncmp(a + sp[1].off, "{\"b\":\"q\\\"}\\\\\",\"c\":[{\"d\":2}]}", sp[1].len) == 0);
        CHECK(strncmp(a + sp[2].off, "{\"a\":\"}{][\"}", sp[2].len) == 0 && sp[2].len == 12);
        for (int i = 0; i < n; i++) {
            jdoc d = { 0 };
            CHECK(json_is_obj(json_parse(&d, a + sp[i].off, sp[i].len, NULL, 0)));
            json_free(&d);
        }
    }
    /* A cut inside a string of the first object, with a backslash run at the cut. */
    const char *b = "\\\\\"}{\"},{\"z\":0}]}";
    n = ctu_ext_scan(b, strlen(b), sp, 16);
    CHECK(n == 1 && sp[0].len == 7);
    /* Empty observations; a cut end; an empty buffer; the "max" limit. */
    const char *e = "{\"schemaVersion\":2,\"observations\":[]}";
    CHECK(ctu_ext_scan(e, strlen(e), sp, 16) == 0);
    const char *cut = "{\"schemaVersion\":2,\"observations\":[{\"a\":1},{\"b\":";
    CHECK(ctu_ext_scan(cut, strlen(cut), sp, 16) == -1);
    CHECK(ctu_ext_scan("", 0, sp, 16) == -1);
    CHECK(ctu_ext_scan("   \r\n", 5, sp, 16) == -1);
    const char *many = "[{},{},{},{}]}";
    CHECK(ctu_ext_scan(many, strlen(many), sp, 2) == 2);
    usage_t u;
    long long obs = 1;
    CHECK(build_text(e, "Fable", 1900000000000LL, &u, &obs) == 0 && obs == 0 && !u.five.present);
    CHECK(build_text(cut, "Fable", 1900000000000LL, &u, &obs) == 0);

    /* The sample file: smaller than the tail, so the whole file is read. */
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\ext\\%ls", g_samples, CTU_EXT_FILE);
    size_t len = 0, flen = 0;
    char *whole = ctu_read_file(p, &flen, NULL);
    char *tail = ctu_read_tail(p, CTU_EXT_TAIL_BYTES, &len);
    CHECK(whole && tail && len == flen && len < CTU_EXT_TAIL_BYTES && memcmp(whole, tail, len) == 0);
    jdoc d = { 0 };
    CHECK(json_is_obj(json_parse(&d, whole, flen, NULL, 0))); /* the sample is valid JSON */
    json_free(&d);
    static ctu_ext_span big[64];
    CHECK(ctu_ext_scan(tail, len, big, 64) == 12); /* 11 records + 1 object inside a record's array */
    const long long T = 1900000000000LL;
    obs = 0;
    CHECK(ctu_ext_build(tail, len, "Fable", T, &u, &obs) == 1);
    CHECK(obs == T - 60000); /* the codex and schemaVersion 3 records at T do not count */
    CHECK(u.five.present && u.five.pct == 57 && u.five.reset_ms == T + 7200000);
    CHECK_STR(u.five.reset_iso, "2030-03-17T19:46:40.000Z");
    CHECK_STR(u.five.key, "five_hour");
    CHECK(u.seven.present && u.seven.pct == 29 && u.seven.reset_ms == T + 432000000);
    CHECK(u.nextra == 1 && u.extra[0].present && u.extra[0].pct == 7);
    CHECK_STR(u.extra[0].key, "weekly_scoped:Fable");
    CHECK_STR(u.extra[0].name, "Weekly Fable");
    CHECK(ctu_find_fable(&u) == &u.extra[0]);
    /* The same set from a tail that cuts the file in the middle of an older record. */
    char *half = ctu_read_tail(p, flen * 2 / 3, &len); /* cuts inside a record of set B */
    CHECK(half && len == flen * 2 / 3 && half[0] != '{');
    usage_t u2;
    long long obs2 = 0;
    CHECK(ctu_ext_build(half, len, "Fable", T, &u2, &obs2) == 1);
    CHECK(obs2 == obs && u2.five.pct == 57 && u2.seven.pct == 29 && u2.nextra == 1 && u2.extra[0].pct == 7);
    /* No label: the scoped records are ignored. */
    CHECK(ctu_ext_build(tail, flen, NULL, T, &u2, &obs2) == 1 && u2.nextra == 0 && u2.five.pct == 57);
    /* A missing file. */
    wchar_t miss[MAX_PATH];
    _snwprintf(miss, MAX_PATH, L"%ls\\ext\\missing.json", g_samples);
    CHECK(ctu_read_tail(miss, CTU_EXT_TAIL_BYTES, &len) == NULL && len == 0);
    free(half);
    free(tail);
    free(whole);
}

static void test_ext_rules(void)
{
    /* Fraction to percent: round to 6 decimals, then floor. */
    CHECK(ctu_ext_pct(0.57) == 57);
    CHECK(ctu_ext_pct(0.29) == 29);
    CHECK(ctu_ext_pct(0.07) == 7);
    CHECK(ctu_ext_pct(1.0) == 100);
    CHECK(ctu_ext_pct(0) == 0);
    CHECK(ctu_ext_pct(0.999) == 99);
    CHECK(ctu_ext_pct(-0.1) == 0);

    /* Labels: exactly one. */
    char lab[40];
    const char *l0 = "{\"schemaVersion\":1,\"labels\":[]}";
    const char *l1 = "{\"schemaVersion\":1,\"labels\":[\"Fable\"]}";
    const char *l2 = "{\"schemaVersion\":1,\"labels\":[\"Fable\",\"Opus\"]}";
    CHECK(ctu_ext_label(l0, strlen(l0), lab, sizeof(lab)) == 0 && lab[0] == 0);
    CHECK(ctu_ext_label(l1, strlen(l1), lab, sizeof(lab)) == 1);
    CHECK_STR(lab, "Fable");
    CHECK(ctu_ext_label(l2, strlen(l2), lab, sizeof(lab)) == 0 && lab[0] == 0);
    CHECK(ctu_ext_label(NULL, 0, lab, sizeof(lab)) == 0);
    CHECK(ctu_ext_label("{bad", 4, lab, sizeof(lab)) == 0);

    const long long T = 1900000000000LL, M = CTU_MIN_MS;
    static char s[16384];
    usage_t u;
    long long obs;
    /* Newest set: per window the newest record. */
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T - 30 * M, 0.10, 0.20, 0.30, T + 60 * M, T + 3000 * M);
    ext_set(s, sizeof(s), T - 2 * M, 0.40, 0.25, 0.35, T + 60 * M, T + 3000 * M);
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 1 && obs == T - 2 * M);
    CHECK(u.five.pct == 40 && u.seven.pct == 25 && u.nextra == 1 && u.extra[0].pct == 35);
    /* 10-minute rule: the newest fetch has no scoped record; the old one (12 min older) does not come back. */
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T - 14 * M, 0.10, 0.20, 0.30, T + 60 * M, T + 3000 * M);
    ext_set(s, sizeof(s), T - 2 * M, 0.40, 0.25, -1, T + 60 * M, T + 3000 * M);
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 1 && u.five.pct == 40 && u.nextra == 0);
    /* Within 10 minutes: the window of the earlier record of the same set is used. */
    ext_begin(s, sizeof(s));
    ext_rec(s, sizeof(s), T - 11 * M, "provider-scoped-seven-day", 0.33, T + 3000 * M, 2, "claude");
    ext_set(s, sizeof(s), T - 2 * M, 0.40, 0.25, -1, T + 60 * M, T + 3000 * M);
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 1 && u.nextra == 1 && u.extra[0].pct == 33);
    /* Expired resetAt: that window is not used. */
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T - 2 * M, 0.90, 0.25, 0.35, T - M, T + 3000 * M);
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 1 && !u.five.present && u.seven.pct == 25);
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T - 2 * M, 0.90, 0.25, 0.35, T - M, T - M);
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 0); /* neither 5h nor W */
    /* Wrong schema version, other provider: ignored. */
    ext_begin(s, sizeof(s));
    ext_rec(s, sizeof(s), T - 2 * M, "five-hour", 0.5, T + 60 * M, 3, "claude");
    ext_rec(s, sizeof(s), T - 2 * M, "seven-day", 0.5, T + 60 * M, 1, "claude");
    ext_rec(s, sizeof(s), T - 2 * M, "seven-day", 0.5, T + 60 * M, 2, "codex");
    ext_end(s, sizeof(s));
    CHECK(build_text(s, "Fable", T, &u, &obs) == 0 && obs == 0);

    /* Dimming rule, each branch (interval 5 min: the limit is 11 min). */
    CHECK(ctu_dim_rule(SRC_EXT, 0, 3 * M, 5, 0) == 0);  /* young */
    CHECK(ctu_dim_rule(SRC_EXT, 0, 12 * M, 5, 0) == 1); /* old, no own 429 */
    CHECK(ctu_dim_rule(SRC_EXT, 1, 12 * M, 5, 1) == 0); /* old, own 429: another tool still polls */
    CHECK(ctu_dim_rule(SRC_EXT, 1, 3 * M, 5, 1) == 0);  /* young, own 429 */
    CHECK(ctu_dim_rule(SRC_EXT, 1, 3 * M, 5, 0) == 0);  /* young, network error: the age rule */
    CHECK(ctu_dim_rule(SRC_EXT, 1, 12 * M, 5, 0) == 1); /* old, network error or login */
    CHECK(ctu_dim_rule(SRC_EXT, 0, 11 * M, 5, 0) == 0); /* exactly at the limit */
    CHECK(ctu_dim_rule(SRC_EXT, 0, 30 * M, 15, 0) == 0 && ctu_dim_rule(SRC_EXT, 0, 32 * M, 15, 0) == 1);
    CHECK(ctu_dim_rule(SRC_LIVE, 1, 3 * M, 5, 0) == 1); /* other sources: a failed attempt dims */
    CHECK(ctu_dim_rule(SRC_LIVE, 1, 3 * M, 5, 1) == 1);
    CHECK(ctu_dim_rule(SRC_CACHE, 0, 12 * M, 5, 1) == 1);
    CHECK(ctu_dim_rule(SRC_CACHE, 0, 3 * M, 5, 0) == 0);
    CHECK(ctu_dim_rule(SRC_NONE, 0, 12 * M, 5, 0) == 1);
}

static void test_ext_engine(void)
{
    const long long M = CTU_MIN_MS;
    const long long T = 1900000000000LL;
    const char *withfable = "{\"five_hour\":{\"utilization\":11,\"resets_at\":\"2030-03-17T20:00:00Z\"},"
                            "\"seven_day\":{\"utilization\":22,\"resets_at\":\"2030-03-22T00:00:00Z\"},"
                            "\"limits\":[{\"kind\":\"weekly_scoped\",\"percent\":33,"
                            "\"scope\":{\"model\":{\"display_name\":\"Fable\"}}}]}";
    char cred[256];
    snprintf(cred, sizeof(cred), "{\"claudeAiOauth\":{\"accessToken\":\"fake-test-token\",\"expiresAt\":%lld}}",
             T + 100000 * M);
    wchar_t d1[CTU_PATH_MAX], d2[CTU_PATH_MAX], d3[CTU_PATH_MAX];
    ext_dir_path(d1, "ext1");
    ext_dir_path(d2, "ext2");
    _snwprintf(d3, CTU_PATH_MAX, L"%ls\\ext-missing", g_scratch);
    ext_write(d1, CTU_EXT_FILE, NULL, 0);
    ext_write(d2, CTU_EXT_FILE, NULL, 0);
    ext_write(d1, CTU_EXT_LABELS, NULL, 0);
    ext_write(d2, CTU_EXT_LABELS, NULL, 0);

    static ctu_engine e;
    fake_http f = { 0, 200, withfable, "" };
    engine_init(&e, 5);
    _snwprintf(e.cred_path, CTU_PATH_MAX, L"%ls\\cred_ext.json", g_scratch);
    _snwprintf(e.last_path, CTU_PATH_MAX, L"%ls\\last_ext.json", g_scratch);
    DeleteFileW(e.last_path);
    write_text(e.cred_path, cred);
    e.fetch = fake_fetch;
    e.fetch_ctx = &f;
    e.use_cc_cache = 0;
    wcscpy(e.ext_dirs[0], d3);
    wcscpy(e.ext_dirs[1], d1);
    wcscpy(e.ext_dirs[2], d2);
    e.n_ext_dirs = 3;

    /* No file anywhere: silent, a request as before. */
    CHECK(engine_check_ext(&e, T) == 0);
    engine_step(&e, T, 0);
    CHECK(f.calls == 1 && e.v.source == SRC_LIVE && e.v.origin == SRC_LIVE && e.v.data.nextra == 1);
    char tip[2048];
    ctu_build_tooltip(&e.v, T + 3 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Updated 3 min ago (live)") != NULL);

    /* Merge: extension newer than live -> used as a whole set (no labels: the live Fable window is gone). */
    static char s[16384];
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T + 2 * M, 0.57, 0.29, 0.07, T + 300 * M, T + 5000 * M);
    ext_end(s, sizeof(s));
    ext_write(d1, CTU_EXT_FILE, s, 133000000000000000LL);
    int calls = f.calls;
    CHECK(engine_step(&e, T + 3 * M, 0) == 1);
    CHECK(f.calls == calls); /* counted as a fetch: no own request */
    CHECK(e.v.source == SRC_EXT && e.v.origin == SRC_EXT && e.v.data_ms == T + 2 * M);
    CHECK(e.v.data.five.pct == 57 && e.v.data.seven.pct == 29 && e.v.data.nextra == 0);
    CHECK(engine_next_due(&e) == T + 7 * M);
    ctu_build_tooltip(&e.v, T + 5 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Updated 3 min ago (VS Code usage extension)") != NULL);
    CHECK(strstr(tip, "Fable") == NULL);
    char line[1024];
    ctu_build_once_line(&e.v, T + 5 * M, line, sizeof(line));
    CHECK(strstr(line, "5h=57%") != NULL && strstr(line, "W=29%") != NULL && strstr(line, "source=extension fetched=") != NULL);

    /* An unchanged file is not read again (data time forced back: still no adoption). */
    long long keep = e.v.data_ms;
    e.v.data_ms = 0;
    CHECK(engine_check_ext(&e, T + 4 * M) == 0);
    /* A new labels file is a change: read again, now with the scoped window. */
    ext_write(d1, CTU_EXT_LABELS, "{\"schemaVersion\":1,\"labels\":[\"Fable\"]}", 0);
    CHECK(engine_check_ext(&e, T + 4 * M) == 1);
    CHECK(e.v.data.nextra == 1 && e.v.data.extra[0].pct == 7);
    CHECK_STR(e.v.data.extra[0].key, "weekly_scoped:Fable");
    CHECK(e.v.data_ms == keep);
    /* Two labels: ambiguous, the scoped window is not used. */
    ext_write(d1, CTU_EXT_LABELS, "{\"schemaVersion\":1,\"labels\":[\"Fable\",\"Opus\"]}", 133000000300000000LL);
    e.v.data_ms = 0;
    CHECK(engine_check_ext(&e, T + 4 * M) == 1 && e.v.data.nextra == 0);
    ext_write(d1, CTU_EXT_LABELS, "{\"schemaVersion\":1,\"labels\":[\"Fable\"]}", 133000000400000000LL);
    e.v.data_ms = 0;
    CHECK(engine_check_ext(&e, T + 4 * M) == 1 && e.v.data.nextra == 1);

    /* last.json round trip with source=extension. */
    size_t len = 0;
    char *saved = ctu_read_file(e.last_path, &len, NULL);
    CHECK(saved && strstr(saved, "\"source\":\"extension\"") != NULL && strstr(saved, "weekly_scoped:Fable") != NULL);
    free(saved);
    static ctu_engine e2;
    engine_init(&e2, 5);
    wcscpy(e2.last_path, e.last_path);
    CHECK(engine_load_saved(&e2) == 1);
    CHECK(e2.v.source == SRC_SAVED && e2.v.origin == SRC_EXT && e2.v.data_ms == T + 2 * M);
    CHECK(e2.v.data.five.pct == 57 && e2.v.data.seven.pct == 29 && e2.v.data.nextra == 1 && e2.v.data.extra[0].pct == 7);
    CHECK(e2.v.data.five.reset_ms == T + 300 * M);
    ctu_build_tooltip(&e2.v, T + 5 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "(VS Code usage extension)") != NULL);
    ctu_build_once_line(&e2.v, T + 5 * M, line, sizeof(line));
    CHECK(strstr(line, "source=saved") != NULL);
    /* An older last.json without "source" still loads. */
    write_text(e2.last_path, "{\"fetchedAtMs\":1794450219575,\"utilization\":{\"five_hour\":{\"utilization\":2,\"resets_at\":null}}}");
    engine_init(&e2, 5);
    wcscpy(e2.last_path, e.last_path);
    CHECK(engine_load_saved(&e2) == 1 && e2.v.origin == SRC_NONE && e2.v.data.five.pct == 2);
    ctu_build_tooltip(&e2.v, 1794450219575LL + 3 * M, 0, tip, sizeof(tip));
    const char *up = strstr(tip, "Updated 3 min ago");
    CHECK(up != NULL && strcmp(up, "Updated 3 min ago") == 0); /* no source text: unknown origin */

    /* Own request at the interval: 429 with extension data -> the friendly note, not dimmed when old. */
    f.status = 429;
    f.body = "";
    calls = f.calls;
    engine_step(&e, T + 7 * M, 0);
    CHECK(f.calls == calls + 1 && e.v.own_429 == 1 && e.v.note_kind == NOTE_RATE && e.v.stale);
    ctu_build_tooltip(&e.v, T + 8 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, CTU_EXT_RATE_NOTE) != NULL && strstr(tip, "Rate limited until") == NULL);
    CHECK(!ctu_view_dimmed(&e.v, T + 8 * M));
    CHECK(!ctu_view_dimmed(&e.v, T + 30 * M)); /* old, but the own request answered 429 */
    /* A non-429 failure: the age rule applies again. */
    f.status = 500;
    f.body = "oops";
    engine_step(&e, T + 40 * M, 0); /* after the 5-min backoff */
    CHECK(e.v.own_429 == 0 && e.v.note_kind == NOTE_FAIL && e.v.origin == SRC_EXT);
    CHECK(ctu_view_dimmed(&e.v, T + 40 * M));
    CHECK(!ctu_view_dimmed(&e.v, T + 2 * M + 11 * M));

    /* The newest candidate file wins: d2 newer than d1. */
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T + 42 * M, 0.61, 0.31, 0.08, T + 300 * M, T + 5000 * M);
    ext_end(s, sizeof(s));
    ext_write(d2, CTU_EXT_FILE, s, 133000000900000000LL);
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T + 43 * M, 0.99, 0.99, 0.99, T + 300 * M, T + 5000 * M);
    ext_end(s, sizeof(s));
    ext_write(d1, CTU_EXT_FILE, s, 133000000800000000LL);
    CHECK(engine_check_ext(&e, T + 44 * M) == 1);
    CHECK(e.v.data.five.pct == 61 && e.v.data_ms == T + 42 * M && e.v.data.nextra == 0); /* d2 has no labels */
    CHECK(e.v.stale == 0 && e.v.note_kind == NOTE_NONE); /* new shared data clears the failure note */
    CHECK(!ctu_view_dimmed(&e.v, T + 44 * M));

    /* Merge: live newer than the extension -> the extension set is not used. */
    f.status = 200;
    f.body = withfable;
    engine_step(&e, T + 50 * M, 0);
    CHECK(e.v.source == SRC_LIVE && e.v.data_ms == T + 50 * M && e.v.data.five.pct == 11 && e.v.own_429 == 0);
    ext_begin(s, sizeof(s));
    ext_set(s, sizeof(s), T + 49 * M, 0.70, 0.40, 0.09, T + 300 * M, T + 5000 * M);
    ext_end(s, sizeof(s));
    ext_write(d2, CTU_EXT_FILE, s, 133000001000000000LL);
    CHECK(engine_check_ext(&e, T + 51 * M) == 0);
    CHECK(e.v.source == SRC_LIVE && e.v.data.five.pct == 11 && e.v.data.nextra == 1);

    /* A cut file, a wrong schema version, a bad file: ignored silently (no note, not stale). */
    ext_write(d2, CTU_EXT_FILE, "{\"schemaVersion\":2,\"observations\":[{\"schemaVersion\":2,\"provi",
              133000001100000000LL);
    CHECK(engine_check_ext(&e, T + 52 * M) == 0 && e.v.note_kind == NOTE_NONE && !e.v.stale);
    ext_begin(s, sizeof(s));
    ext_rec(s, sizeof(s), T + 52 * M, "five-hour", 0.5, T + 300 * M, 3, "claude");
    ext_rec(s, sizeof(s), T + 52 * M, "seven-day", 0.5, T + 5000 * M, 3, "claude");
    ext_end(s, sizeof(s));
    ext_write(d2, CTU_EXT_FILE, s, 133000001200000000LL);
    CHECK(engine_check_ext(&e, T + 53 * M) == 0 && e.v.note_kind == NOTE_NONE && !e.v.stale);
    ext_write(d2, CTU_EXT_FILE, "not json }", 133000001300000000LL);
    CHECK(engine_check_ext(&e, T + 54 * M) == 0 && e.v.source == SRC_LIVE && e.v.data.five.pct == 11);

    /* Tooltip texts of the other sources. */
    ctu_view v = e.v;
    v.source = SRC_CACHE;
    v.origin = SRC_CACHE;
    v.data_ms = T;
    ctu_build_tooltip(&v, T + 3 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Updated 3 min ago (Claude Code cache)") != NULL);
    v.source = SRC_SAMPLE;
    ctu_build_tooltip(&v, T + 3 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Updated 3 min ago (sample file)") != NULL);
    /* A 429 note with data from another source keeps the plain text. */
    v.source = SRC_LIVE;
    v.origin = SRC_LIVE;
    v.note_kind = NOTE_RATE;
    snprintf(v.note, sizeof(v.note), "Rate limited until 1:00 PM.");
    ctu_build_tooltip(&v, T + 3 * M, 0, tip, sizeof(tip));
    CHECK(strstr(tip, "Rate limited until 1:00 PM.") != NULL && strstr(tip, CTU_EXT_RATE_NOTE) == NULL);

    /* The default candidate folders. */
    static ctu_engine e3;
    engine_init(&e3, 5);
    engine_default_ext_dirs(&e3, L"C:\\Users\\x\\AppData\\Roaming");
    CHECK(e3.n_ext_dirs == 5);
    CHECK(wcscmp(e3.ext_dirs[0], L"C:\\Users\\x\\AppData\\Roaming\\Code\\User\\globalStorage\\growthjack.claude-code-usage") == 0);
    CHECK(wcscmp(e3.ext_dirs[1], L"C:\\Users\\x\\AppData\\Roaming\\Code - Insiders\\User\\globalStorage\\growthjack.claude-code-usage") == 0);
    CHECK(wcsstr(e3.ext_dirs[2], L"\\VSCodium\\User\\") != NULL && wcsstr(e3.ext_dirs[3], L"\\Cursor\\User\\") != NULL);
    CHECK(wcsstr(e3.ext_dirs[4], L"\\Antigravity\\User\\globalStorage\\growthjack.claude-code-usage") != NULL);
    engine_default_ext_dirs(&e3, NULL);
    CHECK(e3.n_ext_dirs == 0);

    DeleteFileW(e.cred_path);
    DeleteFileW(e.last_path);
}

/* ---- off-screen render: DirectWrite through Direct2D and the GDI fallback ---------------- */

static void render_case(int gdi, int light, int dim)
{
    render_force_gdi(gdi);
    render_in in;
    memset(&in, 0, sizeof(in));
    in.dpi = 96;
    in.light = light;
    in.height = 48;
    in.n = 3;
    wcscpy(in.label[0], L"5h");
    wcscpy(in.label[1], L"W");
    wcscpy(in.label[2], L"F");
    wcscpy(in.text[0], L"62%");
    wcscpy(in.text[1], L"85%");
    wcscpy(in.text[2], L"97%");
    in.lvl[1] = 1;
    in.lvl[2] = 2;
    in.dim = dim;
    render_out o1, o2;
    memset(&o2, 0, sizeof(o2));
    int ok1 = render_widget(&in, &o1);
    CHECK(ok1);
    if (!ok1) return;
    CHECK(render_last_backend() == (gdi ? RENDER_GDI : RENDER_D2D));
    int ok2 = render_widget(&in, &o2);
    CHECK(ok2 && o2.w == o1.w && o2.h == o1.h); /* the same width for the same values */
    if (ok2 && o2.w == o1.w && o2.h == o1.h) CHECK(memcmp(o1.bits, o2.bits, (size_t)o1.w * o1.h * 4) == 0);
    const DWORD *p = (const DWORD *)o1.bits;
    int w = o1.w, h = o1.h, over = 0, away_bad = 0, text_px = 0, warm = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            DWORD v = p[y * w + x];
            unsigned a = v >> 24, r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            if (r > a || g > a || b > a) over++;
            /* Rows above and below the logo (20 px) and the separators (14 px) around the centre 24. */
            if ((y < 12 || y >= 36) && v != 0x01000000) away_bad++;
            if (x >= 34 && a > 128) text_px++;
            if (x >= 34 && a > 128 && r > g + 40) warm++; /* the orange and red values */
        }
    }
    CHECK(over == 0);     /* no premultiplied channel above its alpha */
    CHECK(away_bad == 0); /* alpha 1 (mouse-catching), no colour, away from text and logo */
    CHECK(text_px > 150);
    CHECK(warm > 20);
    if (over || away_bad || text_px <= 150 || warm <= 20)
        printf("  render case gdi=%d light=%d dim=%d: w %d, over %d, away %d, text %d, warm %d\n", gdi, light, dim, w,
               over, away_bad, text_px, warm);
    DeleteObject(o1.bmp);
    if (ok2) DeleteObject(o2.bmp);
}

static void test_render(void)
{
    for (int gdi = 0; gdi < 2; gdi++)
        for (int light = 0; light < 2; light++) render_case(gdi, light, 0);
    render_case(0, 0, 1);
    render_case(1, 0, 1);

    /* DirectWrite: a real semi-bold face of the variable family, no simulation (no synthetic bold). */
    render_force_gdi(0);
    wchar_t fam[64];
    int wt = 0, sim = -1;
    CHECK(d2d_text_init() && d2d_text_size(12.0f));
    CHECK(d2d_text_face(1, fam, 64, &wt, &sim) && wt == 600 && sim == 0);
    CHECK(wcscmp(fam, L"Segoe UI Variable Text") == 0 || wcscmp(fam, L"Segoe UI") == 0);
    CHECK(d2d_text_face(0, fam, 64, &wt, &sim) && wt == 400 && sim == 0);
    float cap = d2d_text_cap(1);
    CHECK(cap > 7.5f && cap < 9.0f); /* digits 8 px high at 12 px, like the clock */
    CHECK(d2d_text_width(L"88%", 1) > 15 && d2d_text_width(L"88%", 1) < 30);
    /* GDI fallback: the semibold face itself (weight 600), not the regular face emboldened (700). */
    wchar_t face[LF_FACESIZE];
    CHECK(render_gdi_face(1, face, LF_FACESIZE, &wt) && wcsstr(face, L"Semibold") != NULL && wt == 600);
    if (!wcsstr(face, L"Semibold") || wt != 600) printf("  GDI number face %ls weight %d\n", face, wt);
    CHECK(render_gdi_face(0, face, LF_FACESIZE, &wt) && wt == 400);
    render_shutdown();
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: ctu_tests <samples dir> <scratch dir>\n");
        return 2;
    }
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, g_samples, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, g_scratch, MAX_PATH);
    CreateDirectoryW(g_scratch, NULL);
    test_json();
    test_usage();
    test_time();
    test_rules();
    test_engine();
    test_login_retry();
    test_renew();
    test_env_filter();
    test_renew_run();
    test_fable();
    test_fable_saved();
    test_logo();
    test_ext_scan();
    test_ext_rules();
    test_ext_engine();
    test_render();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
