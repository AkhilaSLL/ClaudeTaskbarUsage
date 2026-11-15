/* Shared source "editor extension": the usage records that the VS Code extension
 * growthjack.claude-code-usage saves in quota-observations-v2.json (read only).
 * No window code: the test program links this file. */
#ifndef CTU_EXT_H
#define CTU_EXT_H

#include <stddef.h>
#include <wchar.h>
#include "usage.h"

#define CTU_EXT_TAIL_BYTES 65536            /* only the last 64 KB of the file are read */
#define CTU_EXT_SET_MS (10LL * 60000LL)     /* a window belongs to the newest set when within 10 min */
#define CTU_EXT_FILE L"quota-observations-v2.json"
#define CTU_EXT_LABELS L"quota-scope-labels-v1.json"
#define CTU_EXT_MAX_DIRS 6

typedef struct {
    size_t off, len;
} ctu_ext_span;

/* Finds the complete record objects (objects that are direct elements of an array) in the tail of the
 * file. The scan runs from the end of the buffer (always outside a string) to the front, with string and
 * escape rules, so the cut first object is never completed. Returns the number of spans (newest first),
 * or -1 when the buffer does not end with '}' (a cut or empty file). */
int ctu_ext_scan(const char *buf, size_t len, ctu_ext_span *spans, int max);

/* usedFraction (0..1) to the used percent: x 100, rounded to 6 decimals, floor, minimum 0. */
int ctu_ext_pct(double fraction);

/* Parses the labels file text. Returns 1 and copies the label when it has exactly one string label. */
int ctu_ext_label(const char *text, size_t len, char *out, size_t outlen);

/* Builds the newest set from the tail text: per periodType the record with the largest observedAt,
 * only records of provider "claude", source "claude-official-api", schemaVersion 2; a window only when
 * its observedAt is within 10 min of the newest record and its resetAt is not in the past. The scoped
 * window needs label (NULL = ignore scoped records). Returns 1 when 5h or W is present; observed gets
 * the newest observedAt. */
int ctu_ext_build(const char *tail, size_t len, const char *label, long long now_ms, usage_t *u,
                  long long *observed);

/* Epoch ms to "YYYY-MM-DDTHH:MM:SS.mmmZ". */
void ctu_ms_to_iso(long long ms, char *out, size_t outlen);

/* Reads at most the last CTU_EXT_TAIL_BYTES of a file, opened with full sharing (read, write, delete).
 * Returns a malloc'd NUL-terminated buffer or NULL. */
char *ctu_read_tail(const wchar_t *path, size_t maxbytes, size_t *len);

#endif
