/* Usage data logic: answer parsing, rules, text formatting, threshold colour, backoff.
 * No window code: the test program links this file. */
#ifndef CTU_USAGE_H
#define CTU_USAGE_H

#include <stddef.h>
#include "json.h"

#define CTU_MIN_MS 60000LL
#define CTU_MIN_REQUEST_MS (5 * CTU_MIN_MS)
#define CTU_BACKOFF_MIN_MS (5 * CTU_MIN_MS)
#define CTU_BACKOFF_MAX_MS (30 * CTU_MIN_MS)
#define CTU_MAX_EXTRA 12

typedef struct {
    int present;          /* 1 when utilization was a number */
    int pct;              /* used percent: floor, minimum 0 */
    long long reset_ms;   /* epoch ms, 0 when unknown */
    char reset_iso[40];   /* the resets_at text as received ("" when null) */
    char key[48];         /* answer key, for example "seven_day_opus" */
    char name[48];        /* display name, for example "Weekly Opus" */
} uwin;

typedef struct {
    uwin five;            /* five_hour */
    uwin seven;           /* seven_day */
    uwin extra[CTU_MAX_EXTRA];
    int nextra;
} usage_t;

/* Key prefix of a weekly window scoped to one model, from the "limits" list: "weekly_scoped:Fable". */
#define CTU_SCOPED_PREFIX "weekly_scoped:"

/* 1 when w is a weekly window scoped to a named model (key "weekly_scoped:<model>"). */
int ctu_is_model_scoped(const uwin *w);
/* 1 when w is the weekly window scoped to the model "Fable" (case-insensitive). */
int ctu_is_fable(const uwin *w);
/* The Fable weekly window with a number, or NULL. */
const uwin *ctu_find_fable(const usage_t *u);

/* Taskbar values: "5h", "W" and, when a Fable window is present, "F". */
typedef struct {
    int n;                /* 2 or 3 */
    char label[3][4];
    char text[3][8];      /* ctu_value_text */
    int level[3];         /* ctu_level of the USED value, 0 when absent */
} ctu_bar;
int ctu_bar_values(const usage_t *u, int show_remaining, ctu_bar *bar); /* u may be NULL; returns n */

/* Horizontal layout of the widget, in pixels: pad, icon, gap, then per value label + number, with a
 * separator (gap_sep, sep_w, gap_sep) between two values, and pad at the end. */
typedef struct { int pad, icon, gap_icon, gap_label, gap_sep, sep_w, slot; } ctu_metrics;
typedef struct {
    int n;
    int icon_x;
    int label_x[3], num_x[3];
    int sep_x[2];
    int width;
} ctu_layout;
/* label_w and num_w hold n text widths; a number narrower than slot takes slot. Returns the width. */
int ctu_bar_layout(const ctu_metrics *m, int n, const int *label_w, const int *num_w, ctu_layout *out);

/* Percent rule: floor, minimum 0. Returns 0 when v is not a number (null or absent). */
int usage_pct(const jval *v, int *out);

/* Parses an answer of /api/oauth/usage (or the "utilization" object of a cache).
 * Returns 1 when at least one window has a number.
 * Returns 0 when the answer is an error object or has no window; err gets a reason.
 * err_type (optional) gets error.type of an error object, for example "rate_limit_error". */
int usage_parse(const jval *root, usage_t *u, char *err, size_t errlen, char *err_type, size_t typelen);

/* ISO 8601 (YYYY-MM-DDTHH:MM:SS[.frac][Z|+HH:MM|-HH:MM]) to epoch ms. Returns -1 on error. */
long long iso8601_to_ms(const char *s);

/* Threshold colour level of a USED value: 0 normal, 1 orange (>= 80), 2 red (>= 95). */
int ctu_level(int used_pct);

/* Bar text of one window: "24%" (or 100 - used with show_remaining), "--" when absent. */
void ctu_value_text(const uwin *w, int show_remaining, char *out, size_t outlen);

/* "2 h 13 min", "2 d 4 h", "45 min", "< 1 min" for a positive span; "now" for <= 0. */
void ctu_span_text(long long diff_ms, char *out, size_t outlen);

/* Local clock text: "3:40 PM"; with_day adds the weekday: "Sat 9:00 AM". */
void ctu_clock_text(int hour, int minute, int weekday, int with_day, char *out, size_t outlen);

/* Epoch ms to local time parts (Win32 time zone rules). Returns 0 on error. */
int ctu_local_parts(long long epoch_ms, int *year, int *month, int *day, int *hour, int *minute, int *weekday);

/* "resets 3:40 PM (in 2 h 13 min)"; the weekday is added when the reset is not on today's local date. */
void ctu_reset_text(long long reset_ms, long long now_ms, char *out, size_t outlen);

/* Backoff after HTTP 429: 5, 10, 20, 30, 30 ... minutes; reset on success. */
typedef struct {
    long long until_ms;   /* no request before this time */
    long long next_ms;    /* length of the next backoff */
} backoff_t;
void backoff_init(backoff_t *b);
long long backoff_on_429(backoff_t *b, long long now_ms); /* returns the backoff length used */
void backoff_reset(backoff_t *b);

/* Cache rule: use Claude Code's cache when its fetchedAtMs is newer than our data. */
int ctu_cache_is_newer(long long cache_fetched_ms, long long own_fetched_ms);

/* Credentials rule. Parses .credentials.json text. */
enum { CRED_OK = 0, CRED_MISSING = 1, CRED_EXPIRED = 2 };
/* Copies the access token into tok (may be NULL to only check). Returns CRED_*. */
int ctu_cred_check(const char *text, size_t len, long long now_ms, char *tok, size_t toklen);

/* Current time as epoch ms (Win32 system time). */
long long ctu_now_ms(void);

#endif
