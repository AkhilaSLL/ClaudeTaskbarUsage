/* Data step: Claude Code cache, credentials, request rules, backoff, saved data.
 * No window code: the test program links this file. The HTTP call is a function pointer. */
#ifndef CTU_ENGINE_H
#define CTU_ENGINE_H

#include <wchar.h>
#include "ext.h"
#include "usage.h"

#define CTU_PATH_MAX 520

/* SRC_EXT: the records of the VS Code extension growthjack.claude-code-usage (see ext.h). */
enum { SRC_NONE = 0, SRC_SAVED, SRC_CACHE, SRC_LIVE, SRC_SAMPLE, SRC_EXT };
enum { NOTE_NONE = 0, NOTE_LOGIN, NOTE_RATE, NOTE_FAIL };

#define CTU_LOGIN_NOTE "Login expired. Open Claude Code to renew it."

typedef struct {
    int status;        /* HTTP status; 0 when the transport failed */
    char *body;        /* malloc'd, NUL-terminated (may be NULL) */
    size_t len;
    char err[120];     /* transport error reason when status == 0 */
} ctu_http_result;

/* Sends the request with the token. Fills res. The token must not be stored or logged. */
typedef void (*ctu_fetch_fn)(void *ctx, const char *token, ctu_http_result *res);

/* Renews an expired login (the real hook runs the Claude CLI, see renew.h). Returns when the renewal ended:
 * done, failed or timed out. The engine reads the credentials file again afterwards. */
typedef void (*ctu_renew_fn)(void *ctx);

/* At most one renewal start in this time ("Refresh now" ignores it). */
#define CTU_RENEW_GAP_MS (30LL * 60 * 1000)

/* The part of the state that the UI shows (copied under a lock). */
typedef struct {
    usage_t data;
    int have_data;
    long long data_ms;        /* fetch time of the data (epoch ms) */
    int source;               /* SRC_* (SRC_SAVED after loading last.json) */
    int origin;               /* SRC_* that produced the data (also after loading last.json), SRC_NONE = unknown */
    int own_429;              /* our own last request answered 429 (0 after any other result or no request) */
    int stale;                /* the last attempt failed */
    int note_kind;            /* NOTE_* */
    char note[200];
    char hint[160];           /* answer to "Refresh now" when no request was allowed */
    long long last_attempt_ms;
    long long last_request_ms;
    backoff_t bo;
    int interval_min;
    int requests_made;
} ctu_view;

typedef struct {
    ctu_view v;
    wchar_t cred_path[CTU_PATH_MAX];
    wchar_t ccjson_path[CTU_PATH_MAX];
    wchar_t last_path[CTU_PATH_MAX];    /* "" = do not save */
    long long cc_mtime;                 /* last seen modified time of .claude.json, -1 = never */
    long long cc_checked_ms;
    int use_cc_cache;                   /* 0 in sample mode */
    wchar_t ext_dirs[CTU_EXT_MAX_DIRS][CTU_PATH_MAX]; /* candidate folders of the extension's files */
    int n_ext_dirs;                     /* 0 = the extension source is off */
    wchar_t ext_seen_path[CTU_PATH_MAX];/* last file read, with its time and size (read again only on a change) */
    long long ext_seen_mtime, ext_seen_size, ext_seen_lbl_mtime;
    int skip_credentials;               /* 1 in sample mode */
    long long login_cred_mtime;         /* credentials file time that belongs to the NOTE_LOGIN state, -1 = missing */
    long long login_seen_mtime;         /* credentials file time seen by the last step while NOTE_LOGIN */
    int live_source;                    /* SRC_LIVE or SRC_SAMPLE */
    ctu_fetch_fn fetch;
    void *fetch_ctx;
    ctu_renew_fn renew;                 /* NULL = no renewal */
    void *renew_ctx;
    long long last_renew_ms;            /* start of the last renewal (memory only), -1 = never */
} ctu_engine;

void engine_init(ctu_engine *e, int interval_min);
/* Loads last.json (if last_path is set). Returns 1 when data was loaded. */
int engine_load_saved(ctu_engine *e);
/* Reads Claude Code's cache when its modified time changed; adopts it when newer. Returns 1 when adopted. */
int engine_check_cache(ctu_engine *e, long long now_ms);
/* Reads the extension's records when the newest candidate file (or its labels file) changed; adopts the
 * newest set when it is newer than our data. A missing, cut or unknown file is ignored silently.
 * Returns 1 when adopted. */
int engine_check_ext(ctu_engine *e, long long now_ms);
/* Default candidate folders: %APPDATA%\{Code, Code - Insiders, VSCodium, Cursor, Antigravity}
 * \User\globalStorage\growthjack.claude-code-usage. */
void engine_default_ext_dirs(ctu_engine *e, const wchar_t *appdata);
/* Dimming rule. Extension data: dimmed only when older than 2 intervals + 1 min AND our own last request
 * did not answer 429. Other data: dimmed when the last attempt failed or when older than that. */
int ctu_dim_rule(int origin, int stale, long long age_ms, int interval_min, int own_429);
/* Time of the next scheduled step that may send a request. While NOTE_LOGIN and the credentials file changed
 * (as seen by the last step), the interval does not count; the backoff and the 5 min minimum still do. */
long long engine_next_due(const ctu_engine *e);
/* One data step. force = "Refresh now". Returns 1 when the view changed. */
int engine_step(ctu_engine *e, long long now_ms, int force);
/* Data shown dimmed (ctu_dim_rule with the view's origin, age and own 429). */
int ctu_view_dimmed(const ctu_view *v, long long now_ms);

#define CTU_EXT_RATE_NOTE "Own request rate limited; another tool is using the allowance."
/* " (live)", " (Claude Code cache)", " (VS Code usage extension)", " (sample file)" or "" (by the data's
 * origin; loaded last.json data keeps the origin saved in it). */
const char *ctu_source_suffix(const ctu_view *v);
/* The note to show: CTU_EXT_RATE_NOTE instead of "Rate limited until ..." when the data is from the
 * extension. */
const char *ctu_view_note(const ctu_view *v);
/* Tooltip text (UTF-8, lines separated by \r\n). */
void ctu_build_tooltip(const ctu_view *v, long long now_ms, int show_remaining, char *out, size_t outlen);
/* One line for --once. */
void ctu_build_once_line(const ctu_view *v, long long now_ms, char *out, size_t outlen);

/* Reads a whole file (shared read). Returns a malloc'd NUL-terminated buffer or NULL. mtime may be NULL. */
char *ctu_read_file(const wchar_t *path, size_t *len, long long *mtime);
/* Modified time of a file, -1 when missing. */
long long ctu_file_mtime(const wchar_t *path);

/* Fetch function that reads a sample answer file (test aid CTU_SAMPLE). ctx = const wchar_t *path;
 * the HTTP status comes from the environment variable CTU_SAMPLE_STATUS (default 200). */
void ctu_sample_fetch(void *ctx, const char *token, ctu_http_result *res);

#endif
