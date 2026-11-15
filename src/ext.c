/* Shared source "editor extension": tail scan, record selection, labels (see ext.h). */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext.h"
#include "json.h"

#define MAX_SPANS 2048
#define MAX_DEPTH 64

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    if (!dstlen) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int ctu_ext_scan(const char *b, size_t len, ctu_ext_span *spans, int max)
{
    size_t end = len;
    while (end > 0 && is_space(b[end - 1])) end--;
    if (end == 0 || b[end - 1] != '}') return -1;
    char stack[MAX_DEPTH];
    size_t ends[MAX_DEPTH];
    int rec[MAX_DEPTH];
    int sp = 0, instr = 0, n = 0;
    for (size_t i = end; i-- > 0;) {
        char c = b[i];
        if (instr) {
            /* Going backwards, a quote ends the string unless an odd number of backslashes precedes it. */
            if (c == '"') {
                size_t k = i, bs = 0;
                while (k > 0 && b[k - 1] == '\\') {
                    bs++;
                    k--;
                }
                if (bs % 2 == 0) instr = 0;
            }
            continue;
        }
        if (c == '"') {
            instr = 1; /* outside a string JSON has no backslash: this is a closing quote */
        } else if (c == '}' || c == ']') {
            if (sp >= MAX_DEPTH) break;
            rec[sp] = (c == '}' && sp > 0 && stack[sp - 1] == ']');
            stack[sp] = c;
            ends[sp] = i;
            sp++;
        } else if (c == '{' || c == '[') {
            if (sp == 0) break;                        /* the cut part before the tail */
            sp--;
            if ((c == '{') != (stack[sp] == '}')) break; /* mismatch: only possible in the cut part */
            if (rec[sp] && n < max) {
                spans[n].off = i;
                spans[n].len = ends[sp] - i + 1;
                n++;
            }
        }
    }
    return n;
}

int ctu_ext_pct(double fraction)
{
    double p = fraction * 100.0;
    p = floor(p * 1e6 + 0.5) / 1e6; /* 0.57 * 100 = 56.99999999999999 -> 57 */
    if (!(p > 0)) return 0;         /* also NaN */
    if (p > 1e6) p = 1e6;
    return (int)floor(p);
}

int ctu_ext_label(const char *text, size_t len, char *out, size_t outlen)
{
    if (out && outlen) out[0] = 0;
    if (!text || !len) return 0;
    jdoc d = { 0 };
    jval *r = json_parse(&d, text, len, NULL, 0);
    int ok = 0;
    const jval *labels = json_get(r, "labels");
    if (json_is_arr(labels)) {
        int count = 0;
        const char *one = NULL;
        for (const jval *c = labels->child; c; c = c->next) {
            count++;
            if (json_is_str(c)) one = c->str;
        }
        if (count == 1 && one && one[0]) {
            copy_str(out, outlen, one);
            ok = 1;
        }
    }
    json_free(&d);
    return ok;
}

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
static void civil_from_days(long long z, int *y, int *m, int *d)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

void ctu_ms_to_iso(long long ms, char *out, size_t outlen)
{
    long long days = ms >= 0 ? ms / 86400000LL : -((-ms + 86399999LL) / 86400000LL);
    long long rem = ms - days * 86400000LL;
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    int h = (int)(rem / 3600000LL), mi = (int)(rem / 60000LL % 60), s = (int)(rem / 1000LL % 60),
        msec = (int)(rem % 1000LL);
    snprintf(out, outlen, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, h, mi, s, msec);
}

typedef struct {
    int have;
    long long observed;
    long long reset;
    double fraction;
} best_rec;

static int str_is(const jval *v, const char *s)
{
    const char *t = json_str(v);
    return t && strcmp(t, s) == 0;
}

static void fill(uwin *w, const best_rec *b, const char *key, const char *name)
{
    memset(w, 0, sizeof(*w));
    w->present = 1;
    w->pct = ctu_ext_pct(b->fraction);
    w->reset_ms = b->reset > 0 ? b->reset : 0;
    if (w->reset_ms) ctu_ms_to_iso(w->reset_ms, w->reset_iso, sizeof(w->reset_iso));
    copy_str(w->key, sizeof(w->key), key);
    copy_str(w->name, sizeof(w->name), name);
}

int ctu_ext_build(const char *tail, size_t len, const char *label, long long now_ms, usage_t *u,
                  long long *observed)
{
    memset(u, 0, sizeof(*u));
    copy_str(u->five.name, sizeof(u->five.name), "5-hour");
    copy_str(u->five.key, sizeof(u->five.key), "five_hour");
    copy_str(u->seven.name, sizeof(u->seven.name), "Weekly");
    copy_str(u->seven.key, sizeof(u->seven.key), "seven_day");
    if (observed) *observed = 0;
    if (!tail) return 0;
    ctu_ext_span *spans = (ctu_ext_span *)malloc(sizeof(ctu_ext_span) * MAX_SPANS);
    if (!spans) return 0;
    int n = ctu_ext_scan(tail, len, spans, MAX_SPANS);
    best_rec best[3];
    memset(best, 0, sizeof(best));
    long long newest = 0;
    for (int i = 0; i < n; i++) {
        jdoc d = { 0 };
        jval *r = json_parse(&d, tail + spans[i].off, spans[i].len, NULL, 0);
        const jval *sv = json_get(r, "schemaVersion");
        const jval *ob = json_get(r, "observedAt");
        const jval *uf = json_get(r, "usedFraction");
        const jval *rs = json_get(r, "resetAt");
        const char *pt = json_str(json_get(r, "periodType"));
        int kind = -1;
        if (pt) {
            if (strcmp(pt, "five-hour") == 0) kind = 0;
            else if (strcmp(pt, "seven-day") == 0) kind = 1;
            else if (strcmp(pt, "provider-scoped-seven-day") == 0 && label) kind = 2;
        }
        if (json_is_obj(r) && kind >= 0 && json_is_num(sv) && sv->num == 2 && str_is(json_get(r, "provider"), "claude") &&
            str_is(json_get(r, "source"), "claude-official-api") && json_is_num(ob) && ob->num > 0 && json_is_num(uf)) {
            long long t = (long long)ob->num;
            if (t > newest) newest = t;
            if (!best[kind].have || t > best[kind].observed) {
                best[kind].have = 1;
                best[kind].observed = t;
                best[kind].fraction = uf->num;
                best[kind].reset = json_is_num(rs) ? (long long)rs->num : 0;
            }
        }
        json_free(&d);
    }
    free(spans);
    for (int k = 0; k < 3; k++) {
        best_rec *b = &best[k];
        if (!b->have) continue;
        if (b->observed < newest - CTU_EXT_SET_MS) continue; /* not part of the newest fetch */
        if (b->reset > 0 && b->reset <= now_ms) continue;     /* the window has already reset */
        if (k == 0) {
            fill(&u->five, b, "five_hour", "5-hour");
        } else if (k == 1) {
            fill(&u->seven, b, "seven_day", "Weekly");
        } else {
            char key[64], name[64];
            snprintf(key, sizeof(key), "%s%s", CTU_SCOPED_PREFIX, label);
            snprintf(name, sizeof(name), "Weekly %s", label);
            fill(&u->extra[0], b, key, name);
            u->nextra = 1;
        }
    }
    if (observed) *observed = newest;
    return u->five.present || u->seven.present;
}

char *ctu_read_tail(const wchar_t *path, size_t maxbytes, size_t *len)
{
    if (len) *len = 0;
    if (!path || !path[0]) return NULL;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = sz.QuadPart > (LONGLONG)maxbytes ? maxbytes : (size_t)sz.QuadPart;
    LARGE_INTEGER pos;
    pos.QuadPart = sz.QuadPart - (LONGLONG)n;
    char *buf = (char *)malloc(n + 1);
    if (!buf || !SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    DWORD got = 0;
    size_t total = 0;
    while (total < n) {
        if (!ReadFile(h, buf + total, (DWORD)(n - total), &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    buf[total] = 0;
    if (len) *len = total;
    return buf;
}
