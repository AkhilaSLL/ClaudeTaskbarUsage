/* Usage data logic: answer parsing, rules, text formatting, threshold colour, backoff. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "usage.h"

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    if (!dstlen) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int usage_pct(const jval *v, int *out)
{
    if (!json_is_num(v)) return 0;
    double d = v->num;
    if (d != d) return 0; /* NaN */
    d = floor(d);
    if (d < 0) d = 0;
    if (d > 100000) d = 100000;
    *out = (int)d;
    return 1;
}

/* Fills w from an object { utilization, resets_at }. Returns 1 when utilization is a number. */
static int fill_window(uwin *w, const jval *obj, const char *key, const char *name, const char *pct_member)
{
    memset(w, 0, sizeof(*w));
    copy_str(w->key, sizeof(w->key), key);
    const char *own = json_str(json_get(obj, "name"));
    copy_str(w->name, sizeof(w->name), own ? own : name);
    const char *iso = json_str(json_get(obj, "resets_at"));
    if (iso) {
        copy_str(w->reset_iso, sizeof(w->reset_iso), iso);
        long long ms = iso8601_to_ms(iso);
        w->reset_ms = ms > 0 ? ms : 0;
    }
    w->present = usage_pct(json_get(obj, pct_member), &w->pct);
    return w->present;
}

/* Display name for an extra key: "seven_day_opus" -> "Weekly Opus". */
static void extra_name(const char *key, char *out, size_t outlen)
{
    static const struct { const char *key; const char *name; } known[] = {
        { "seven_day_opus", "Weekly Opus" },
        { "seven_day_sonnet", "Weekly Sonnet" },
        { "seven_day_oauth_apps", "Weekly OAuth apps" },
        { "seven_day_cowork", "Weekly Cowork" },
        { "extra_usage", "Extra usage" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(key, known[i].key) == 0) {
            copy_str(out, outlen, known[i].name);
            return;
        }
    }
    char tmp[96];
    const char *rest = key;
    size_t n = 0;
    if (strncmp(key, "seven_day_", 10) == 0) {
        copy_str(tmp, sizeof(tmp), "Weekly ");
        n = strlen(tmp);
        rest = key + 10;
    } else if (strncmp(key, "five_hour_", 10) == 0) {
        copy_str(tmp, sizeof(tmp), "5-hour ");
        n = strlen(tmp);
        rest = key + 10;
    }
    int first = (n == 0);
    for (; *rest && n + 1 < sizeof(tmp); rest++) {
        char c = *rest == '_' ? ' ' : *rest;
        if (first && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        first = 0;
        tmp[n++] = c;
    }
    tmp[n] = 0;
    copy_str(out, outlen, tmp);
}

/* Adds an extra window; a key that is already in the list is not added a second time. */
static void add_extra(usage_t *u, const uwin *w)
{
    for (int i = 0; i < u->nextra; i++)
        if (strcmp(u->extra[i].key, w->key) == 0) return;
    if (u->nextra < CTU_MAX_EXTRA) u->extra[u->nextra++] = *w;
}

/* Name of a scope object part: { "display_name": ... }, or a plain string, or { "id": ... }. */
static const char *scope_part_name(const jval *part)
{
    if (json_is_str(part)) return part->str;
    const char *s = json_str(json_get(part, "display_name"));
    if (!s) s = json_str(json_get(part, "name"));
    if (!s) s = json_str(json_get(part, "id"));
    return (s && s[0]) ? s : NULL;
}

/* The "limits" list of the answer. Read always, next to the top-level keys:
 *  - session and weekly_all fill five_hour and seven_day only when the top-level keys gave no number;
 *  - weekly_scoped with a model name: key "weekly_scoped:<model>", name "Weekly <model>";
 *  - weekly_scoped without a model name (for example a surface scope): key "weekly_scoped_other:<scope>",
 *    name "Weekly (<scope>)", tooltip only;
 *  - other kinds: key "limit_<kind>[:<scope>]". */
static void parse_limits(const jval *limits, usage_t *u)
{
    for (const jval *row = limits->child; row; row = row->next) {
        if (!json_is_obj(row)) continue;
        const char *pm = json_is_num(json_get(row, "percent")) ? "percent" : "utilization";
        const char *kind = json_str(json_get(row, "kind"));
        if (!kind || !kind[0]) kind = "limit";
        const jval *scope = json_get(row, "scope");
        const char *model = scope_part_name(json_get(scope, "model"));
        const char *other = scope_part_name(json_get(scope, "surface"));
        uwin w;
        if (strcmp(kind, "session") == 0) {
            if (!u->five.present) fill_window(&u->five, row, "five_hour", "5-hour", pm);
            continue;
        }
        if (strcmp(kind, "weekly_all") == 0) {
            if (!u->seven.present) fill_window(&u->seven, row, "seven_day", "Weekly", pm);
            continue;
        }
        char name[64];
        char key[64];
        if (strcmp(kind, "weekly_scoped") == 0) {
            if (model) {
                snprintf(name, sizeof(name), "Weekly %s", model);
                snprintf(key, sizeof(key), "%s%s", CTU_SCOPED_PREFIX, model);
            } else {
                snprintf(name, sizeof(name), "Weekly (%s)", other ? other : "scoped");
                snprintf(key, sizeof(key), "weekly_scoped_other:%s", other ? other : "scoped");
            }
        } else {
            char kname[48];
            extra_name(kind, kname, sizeof(kname));
            const char *sn = model ? model : other;
            if (sn) {
                snprintf(name, sizeof(name), "%s (%s)", kname, sn);
                snprintf(key, sizeof(key), "limit_%s:%s", kind, sn);
            } else {
                copy_str(name, sizeof(name), kname);
                snprintf(key, sizeof(key), "limit_%s", kind);
            }
        }
        if (fill_window(&w, row, key, name, pm)) add_extra(u, &w);
    }
}

int ctu_is_model_scoped(const uwin *w)
{
    size_t n = strlen(CTU_SCOPED_PREFIX);
    return w && strncmp(w->key, CTU_SCOPED_PREFIX, n) == 0 && w->key[n] != 0;
}

int ctu_is_fable(const uwin *w)
{
    return ctu_is_model_scoped(w) && _stricmp(w->key + strlen(CTU_SCOPED_PREFIX), "Fable") == 0;
}

const uwin *ctu_find_fable(const usage_t *u)
{
    if (!u) return NULL;
    for (int i = 0; i < u->nextra; i++)
        if (ctu_is_fable(&u->extra[i]) && u->extra[i].present) return &u->extra[i];
    return NULL;
}

/* Tooltip order of the extras: Fable first (directly after "Weekly"), then other model-scoped weekly
 * windows, then the rest; stable within each group. */
static int extra_rank(const uwin *w)
{
    if (ctu_is_fable(w)) return 0;
    if (ctu_is_model_scoped(w)) return 1;
    return 2;
}

static void order_extras(usage_t *u)
{
    for (int i = 1; i < u->nextra; i++) {
        uwin t = u->extra[i];
        int r = extra_rank(&t);
        int j = i - 1;
        while (j >= 0 && extra_rank(&u->extra[j]) > r) {
            u->extra[j + 1] = u->extra[j];
            j--;
        }
        u->extra[j + 1] = t;
    }
}

int ctu_bar_values(const usage_t *u, int show_remaining, ctu_bar *bar)
{
    memset(bar, 0, sizeof(*bar));
    const uwin *w[3] = { u ? &u->five : NULL, u ? &u->seven : NULL, ctu_find_fable(u) };
    static const char *labels[3] = { "5h", "W", "F" };
    int n = w[2] ? 3 : 2;
    for (int i = 0; i < n; i++) {
        copy_str(bar->label[i], sizeof(bar->label[i]), labels[i]);
        ctu_value_text(w[i], show_remaining, bar->text[i], sizeof(bar->text[i]));
        bar->level[i] = (w[i] && w[i]->present) ? ctu_level(w[i]->pct) : 0;
    }
    bar->n = n;
    return n;
}

int ctu_bar_layout(const ctu_metrics *m, int n, const int *label_w, const int *num_w, ctu_layout *out)
{
    memset(out, 0, sizeof(*out));
    if (n < 1) n = 1;
    if (n > 3) n = 3;
    int x = m->pad;
    out->icon_x = x;
    x += m->icon + m->gap_icon;
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            x += m->gap_sep;
            out->sep_x[i - 1] = x;
            x += m->sep_w + m->gap_sep;
        }
        out->label_x[i] = x;
        x += label_w[i] + m->gap_label;
        out->num_x[i] = x;
        x += (num_w[i] > m->slot ? num_w[i] : m->slot);
    }
    x += m->pad;
    out->n = n;
    out->width = x;
    return x;
}

int usage_parse(const jval *root, usage_t *u, char *err, size_t errlen, char *err_type, size_t typelen)
{
    memset(u, 0, sizeof(*u));
    copy_str(u->five.name, sizeof(u->five.name), "5-hour");
    copy_str(u->five.key, sizeof(u->five.key), "five_hour");
    copy_str(u->seven.name, sizeof(u->seven.name), "Weekly");
    copy_str(u->seven.key, sizeof(u->seven.key), "seven_day");
    if (err && errlen) err[0] = 0;
    if (err_type && typelen) err_type[0] = 0;
    if (!json_is_obj(root)) {
        copy_str(err, errlen, "answer is not a JSON object");
        return 0;
    }
    const jval *e = json_get(root, "error");
    const char *rtype = json_str(json_get(root, "type"));
    if (json_is_obj(e) || (rtype && strcmp(rtype, "error") == 0)) {
        const char *msg = json_str(json_get(e, "message"));
        const char *et = json_str(json_get(e, "type"));
        if (!msg && json_is_str(e)) msg = e->str;
        copy_str(err, errlen, msg ? msg : "error answer");
        copy_str(err_type, typelen, et ? et : "");
        return 0;
    }
    const jval *fh = json_get(root, "five_hour");
    const jval *sd = json_get(root, "seven_day");
    if (json_is_obj(fh)) fill_window(&u->five, fh, "five_hour", "5-hour", "utilization");
    if (json_is_obj(sd)) fill_window(&u->seven, sd, "seven_day", "Weekly", "utilization");
    for (const jval *c = root->child; c; c = c->next) {
        if (!c->key || !json_is_obj(c)) continue;
        if (strcmp(c->key, "five_hour") == 0 || strcmp(c->key, "seven_day") == 0) continue;
        if (!json_is_num(json_get(c, "utilization"))) continue;
        char name[48];
        extra_name(c->key, name, sizeof(name));
        uwin w;
        if (fill_window(&w, c, c->key, name, "utilization")) add_extra(u, &w);
    }
    const jval *limits = json_get(root, "limits");
    if (json_is_arr(limits)) parse_limits(limits, u);
    order_extras(u);
    if (!u->five.present && !u->seven.present && u->nextra == 0) {
        copy_str(err, errlen, "the answer has no usage window");
        return 0;
    }
    return 1;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm). */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static int read_digits(const char **p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return 1;
}

long long iso8601_to_ms(const char *s)
{
    if (!s) return -1;
    const char *p = s;
    int y, mo, d, h, mi, sec = 0, ms = 0;
    if (!read_digits(&p, 4, &y) || *p++ != '-') return -1;
    if (!read_digits(&p, 2, &mo) || *p++ != '-') return -1;
    if (!read_digits(&p, 2, &d)) return -1;
    if (*p != 'T' && *p != 't' && *p != ' ') return -1;
    p++;
    if (!read_digits(&p, 2, &h) || *p++ != ':') return -1;
    if (!read_digits(&p, 2, &mi)) return -1;
    if (*p == ':') {
        p++;
        if (!read_digits(&p, 2, &sec)) return -1;
        if (*p == '.' || *p == ',') {
            p++;
            int digits = 0;
            while (*p >= '0' && *p <= '9') {
                if (digits < 3) ms = ms * 10 + (*p - '0');
                digits++;
                p++;
            }
            if (digits == 0) return -1;
            while (digits < 3) {
                ms *= 10;
                digits++;
            }
        }
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return -1;
    long long offset_min = 0;
    if (*p == 'Z' || *p == 'z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        p++;
        int oh, om = 0;
        if (!read_digits(&p, 2, &oh)) return -1;
        if (*p == ':') p++;
        if (*p >= '0' && *p <= '9') {
            if (!read_digits(&p, 2, &om)) return -1;
        }
        offset_min = sign * (oh * 60 + om);
    } else if (*p != 0) {
        return -1;
    }
    if (*p != 0) return -1;
    long long days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    long long t = ((days * 24 + h) * 60 + mi) * 60 + sec;
    t -= offset_min * 60;
    return t * 1000 + ms;
}

int ctu_level(int used_pct)
{
    if (used_pct >= 95) return 2;
    if (used_pct >= 80) return 1;
    return 0;
}

void ctu_value_text(const uwin *w, int show_remaining, char *out, size_t outlen)
{
    if (!w || !w->present) {
        copy_str(out, outlen, "--");
        return;
    }
    int v = w->pct;
    if (show_remaining) {
        v = 100 - v;
        if (v < 0) v = 0;
    }
    snprintf(out, outlen, "%d%%", v);
}

void ctu_span_text(long long diff_ms, char *out, size_t outlen)
{
    if (diff_ms <= 0) {
        copy_str(out, outlen, "now");
        return;
    }
    long long m = diff_ms / 60000;
    if (m < 1) {
        copy_str(out, outlen, "< 1 min");
        return;
    }
    long long d = m / 1440;
    long long h = (m % 1440) / 60;
    long long mm = m % 60;
    if (d > 0) {
        if (h > 0)
            snprintf(out, outlen, "%lld d %lld h", d, h);
        else
            snprintf(out, outlen, "%lld d", d);
    } else if (h > 0) {
        if (mm > 0)
            snprintf(out, outlen, "%lld h %lld min", h, mm);
        else
            snprintf(out, outlen, "%lld h", h);
    } else {
        snprintf(out, outlen, "%lld min", mm);
    }
}

void ctu_clock_text(int hour, int minute, int weekday, int with_day, char *out, size_t outlen)
{
    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    const char *ampm = hour < 12 ? "AM" : "PM";
    if (with_day && weekday >= 0 && weekday < 7)
        snprintf(out, outlen, "%s %d:%02d %s", days[weekday], h12, minute, ampm);
    else
        snprintf(out, outlen, "%d:%02d %s", h12, minute, ampm);
}

int ctu_local_parts(long long epoch_ms, int *year, int *month, int *day, int *hour, int *minute, int *weekday)
{
    if (epoch_ms < 0) return 0;
    ULONGLONG ft64 = (ULONGLONG)epoch_ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(ft64 & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)(ft64 >> 32);
    SYSTEMTIME utc, loc;
    if (!FileTimeToSystemTime(&ft, &utc)) return 0;
    if (!SystemTimeToTzSpecificLocalTimeEx(NULL, &utc, &loc)) return 0;
    if (year) *year = loc.wYear;
    if (month) *month = loc.wMonth;
    if (day) *day = loc.wDay;
    if (hour) *hour = loc.wHour;
    if (minute) *minute = loc.wMinute;
    if (weekday) *weekday = loc.wDayOfWeek;
    return 1;
}

void ctu_reset_text(long long reset_ms, long long now_ms, char *out, size_t outlen)
{
    if (reset_ms <= 0) {
        copy_str(out, outlen, "");
        return;
    }
    int y, mo, d, h, mi, wd;
    int ny, nmo, nd;
    if (!ctu_local_parts(reset_ms, &y, &mo, &d, &h, &mi, &wd) ||
        !ctu_local_parts(now_ms, &ny, &nmo, &nd, NULL, NULL, NULL)) {
        copy_str(out, outlen, "");
        return;
    }
    int other_day = (y != ny || mo != nmo || d != nd);
    char clock[32], span[32];
    ctu_clock_text(h, mi, wd, other_day, clock, sizeof(clock));
    if (reset_ms - now_ms <= 0) {
        snprintf(out, outlen, "resets %s (due now)", clock);
        return;
    }
    ctu_span_text(reset_ms - now_ms, span, sizeof(span));
    snprintf(out, outlen, "resets %s (in %s)", clock, span);
}

void backoff_init(backoff_t *b)
{
    b->until_ms = 0;
    b->next_ms = CTU_BACKOFF_MIN_MS;
}

long long backoff_on_429(backoff_t *b, long long now_ms)
{
    long long used = b->next_ms;
    b->until_ms = now_ms + used;
    b->next_ms = used * 2;
    if (b->next_ms > CTU_BACKOFF_MAX_MS) b->next_ms = CTU_BACKOFF_MAX_MS;
    return used;
}

void backoff_reset(backoff_t *b)
{
    b->until_ms = 0;
    b->next_ms = CTU_BACKOFF_MIN_MS;
}

int ctu_cache_is_newer(long long cache_fetched_ms, long long own_fetched_ms)
{
    return cache_fetched_ms > 0 && cache_fetched_ms > own_fetched_ms;
}

int ctu_cred_check(const char *text, size_t len, long long now_ms, char *tok, size_t toklen)
{
    if (tok && toklen) tok[0] = 0;
    if (!text || !len) return CRED_MISSING;
    jdoc doc = { 0 };
    jval *root = json_parse(&doc, text, len, NULL, 0);
    const jval *oauth = json_get(root, "claudeAiOauth");
    const char *at = json_str(json_get(oauth, "accessToken"));
    int rc;
    if (!at || !at[0]) {
        rc = CRED_MISSING;
    } else {
        const jval *exp = json_get(oauth, "expiresAt");
        if (json_is_num(exp) && (double)now_ms >= exp->num) {
            rc = CRED_EXPIRED;
        } else if (tok && toklen) {
            size_t n = strlen(at);
            if (n >= toklen) {
                rc = CRED_MISSING; /* does not fit: treat as unusable */
            } else {
                memcpy(tok, at, n + 1);
                rc = CRED_OK;
            }
        } else {
            rc = CRED_OK;
        }
    }
    json_free(&doc);
    return rc;
}

long long ctu_now_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)((t - 116444736000000000ULL) / 10000ULL);
}
