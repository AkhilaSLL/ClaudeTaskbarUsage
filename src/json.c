/* Small JSON parser (recursive descent) with an arena allocator. */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JBLOCK_SIZE (64 * 1024)
#define JMAX_DEPTH 512

struct jblock {
    jblock *next;
    size_t used;
    size_t size;
    /* data follows */
};

static void *jalloc(jdoc *doc, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    jblock *b = doc->blocks;
    if (!b || b->size - b->used < n) {
        size_t sz = n > JBLOCK_SIZE ? n : JBLOCK_SIZE;
        jblock *nb = (jblock *)malloc(sizeof(jblock) + 16 + sz);
        if (!nb) return NULL;
        nb->used = 0;
        nb->size = sz;
        if (b && n > JBLOCK_SIZE) {
            /* Keep the current block on top for small allocations. */
            nb->next = b->next;
            b->next = nb;
        } else {
            nb->next = b;
            doc->blocks = nb;
        }
        b = nb;
    }
    char *base = (char *)(b + 1);
    base = (char *)(((size_t)base + 15) & ~(size_t)15);
    void *p = base + b->used;
    b->used += n;
    return p;
}

void json_free(jdoc *doc)
{
    jblock *b = doc->blocks;
    while (b) {
        jblock *n = b->next;
        /* Wipe the parsed text (the credentials file holds a token). */
        memset(b + 1, 0, 16 + b->used);
        free(b);
        b = n;
    }
    doc->blocks = NULL;
}

typedef struct {
    jdoc *doc;
    const char *p;
    const char *end;
    const char *start;
    char *err;
    size_t errlen;
    int failed;
} jparser;

static void jfail(jparser *ps, const char *msg)
{
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->err && ps->errlen)
        snprintf(ps->err, ps->errlen, "%s at offset %lu", msg, (unsigned long)(ps->p - ps->start));
}

static void skip_ws(jparser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

static jval *new_val(jparser *ps, jtype t)
{
    jval *v = (jval *)jalloc(ps->doc, sizeof(jval));
    if (!v) {
        jfail(ps, "out of memory");
        return NULL;
    }
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static size_t put_utf8(char *o, unsigned cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string at ps->p (which points at the opening quote). */
static char *parse_string(jparser *ps, size_t *outlen)
{
    ps->p++; /* opening quote */
    const char *s = ps->p;
    /* Find the end first, to size the output (decoded text is never longer). */
    const char *q = s;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            q++;
            if (q >= ps->end) break;
        } else if ((unsigned char)*q < 0x20) {
            ps->p = q;
            jfail(ps, "control character in string");
            return NULL;
        }
        q++;
    }
    if (q >= ps->end) {
        jfail(ps, "unterminated string");
        return NULL;
    }
    char *out = (char *)jalloc(ps->doc, (size_t)(q - s) + 1);
    if (!out) {
        jfail(ps, "out of memory");
        return NULL;
    }
    size_t n = 0;
    const char *r = s;
    while (r < q) {
        char c = *r++;
        if (c != '\\') {
            out[n++] = c;
            continue;
        }
        char e = *r++;
        switch (e) {
        case '"': out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; break;
        case '/': out[n++] = '/'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (q - r < 4 || !hex4(r, &cp)) {
                ps->p = r;
                jfail(ps, "bad \\u escape");
                return NULL;
            }
            r += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (q - r >= 6 && r[0] == '\\' && r[1] == 'u' && hex4(r + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    r += 6;
                } else {
                    cp = 0xFFFD; /* lone high surrogate */
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD; /* lone low surrogate */
            }
            /* A \uXXXX escape takes 6 input bytes and gives at most 4 output bytes
             * (a surrogate pair takes 12 and gives 4), so the buffer is large enough. */
            n += put_utf8(out + n, cp);
            break;
        }
        default:
            ps->p = r - 1;
            jfail(ps, "bad escape");
            return NULL;
        }
    }
    out[n] = 0;
    *outlen = n;
    ps->p = q + 1;
    return out;
}

static jval *parse_value(jparser *ps, int depth);

static jval *parse_number(jparser *ps)
{
    const char *s = ps->p;
    const char *q = s;
    if (q < ps->end && *q == '-') q++;
    if (q >= ps->end || !(*q >= '0' && *q <= '9')) {
        jfail(ps, "bad number");
        return NULL;
    }
    if (*q == '0') {
        q++;
    } else {
        while (q < ps->end && *q >= '0' && *q <= '9') q++;
    }
    if (q < ps->end && *q == '.') {
        q++;
        if (q >= ps->end || !(*q >= '0' && *q <= '9')) {
            ps->p = q;
            jfail(ps, "bad fraction");
            return NULL;
        }
        while (q < ps->end && *q >= '0' && *q <= '9') q++;
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < ps->end && (*q == '+' || *q == '-')) q++;
        if (q >= ps->end || !(*q >= '0' && *q <= '9')) {
            ps->p = q;
            jfail(ps, "bad exponent");
            return NULL;
        }
        while (q < ps->end && *q >= '0' && *q <= '9') q++;
    }
    char buf[64];
    size_t n = (size_t)(q - s);
    jval *v = new_val(ps, J_NUM);
    if (!v) return NULL;
    if (n < sizeof(buf)) {
        memcpy(buf, s, n);
        buf[n] = 0;
        v->num = strtod(buf, NULL);
    } else {
        char *tmp = (char *)malloc(n + 1);
        if (!tmp) {
            jfail(ps, "out of memory");
            return NULL;
        }
        memcpy(tmp, s, n);
        tmp[n] = 0;
        v->num = strtod(tmp, NULL);
        free(tmp);
    }
    ps->p = q;
    return v;
}

static int match_lit(jparser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, lit, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static jval *parse_array(jparser *ps, int depth)
{
    jval *arr = new_val(ps, J_ARR);
    if (!arr) return NULL;
    ps->p++; /* [ */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }
    jval **tail = &arr->child;
    for (;;) {
        skip_ws(ps);
        jval *v = parse_value(ps, depth + 1);
        if (!v) return NULL;
        *tail = v;
        tail = &v->next;
        arr->len++;
        skip_ws(ps);
        if (ps->p >= ps->end) {
            jfail(ps, "unterminated array");
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        jfail(ps, "expected , or ]");
        return NULL;
    }
}

static jval *parse_object(jparser *ps, int depth)
{
    jval *obj = new_val(ps, J_OBJ);
    if (!obj) return NULL;
    ps->p++; /* { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }
    jval **tail = &obj->child;
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            jfail(ps, "expected member name");
            return NULL;
        }
        size_t klen = 0;
        char *key = parse_string(ps, &klen);
        if (!key) return NULL;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            jfail(ps, "expected :");
            return NULL;
        }
        ps->p++;
        skip_ws(ps);
        jval *v = parse_value(ps, depth + 1);
        if (!v) return NULL;
        v->key = key;
        *tail = v;
        tail = &v->next;
        obj->len++;
        skip_ws(ps);
        if (ps->p >= ps->end) {
            jfail(ps, "unterminated object");
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        jfail(ps, "expected , or }");
        return NULL;
    }
}

static jval *parse_value(jparser *ps, int depth)
{
    if (depth > JMAX_DEPTH) {
        jfail(ps, "nesting too deep");
        return NULL;
    }
    skip_ws(ps);
    if (ps->p >= ps->end) {
        jfail(ps, "unexpected end");
        return NULL;
    }
    char c = *ps->p;
    switch (c) {
    case '{': return parse_object(ps, depth);
    case '[': return parse_array(ps, depth);
    case '"': {
        jval *v = new_val(ps, J_STR);
        if (!v) return NULL;
        v->str = parse_string(ps, &v->len);
        return v->str ? v : NULL;
    }
    case 't':
        if (match_lit(ps, "true")) {
            jval *v = new_val(ps, J_BOOL);
            if (v) v->boolean = 1;
            return v;
        }
        break;
    case 'f':
        if (match_lit(ps, "false")) return new_val(ps, J_BOOL);
        break;
    case 'n':
        if (match_lit(ps, "null")) return new_val(ps, J_NULL);
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
        break;
    }
    jfail(ps, "unexpected character");
    return NULL;
}

jval *json_parse(jdoc *doc, const char *text, size_t len, char *err, size_t errlen)
{
    jparser ps;
    ps.doc = doc;
    ps.p = text;
    ps.end = text + len;
    ps.start = text;
    ps.err = err;
    ps.errlen = errlen;
    ps.failed = 0;
    if (err && errlen) err[0] = 0;
    /* Skip a UTF-8 byte order mark. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    jval *root = parse_value(&ps, 0);
    if (!root) {
        jfail(&ps, "parse error");
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        jfail(&ps, "trailing characters");
        return NULL;
    }
    return root;
}

const jval *json_get(const jval *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (const jval *c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

int json_is_num(const jval *v) { return v && v->type == J_NUM; }
int json_is_str(const jval *v) { return v && v->type == J_STR; }
int json_is_obj(const jval *v) { return v && v->type == J_OBJ; }
int json_is_arr(const jval *v) { return v && v->type == J_ARR; }
double json_num(const jval *v, double def) { return (v && v->type == J_NUM) ? v->num : def; }
const char *json_str(const jval *v) { return (v && v->type == J_STR) ? v->str : NULL; }
