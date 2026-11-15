/* Small JSON parser (recursive descent) with an arena allocator.
 * No window code: the test program links this file. */
#ifndef CTU_JSON_H
#define CTU_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval jval;
struct jval {
    jtype type;
    int boolean;      /* J_BOOL */
    double num;       /* J_NUM */
    char *str;        /* J_STR: UTF-8, NUL-terminated */
    size_t len;       /* J_STR: length in bytes; J_ARR/J_OBJ: number of children */
    jval *child;      /* J_ARR/J_OBJ: first child */
    jval *next;       /* next sibling */
    char *key;        /* member name when the parent is an object */
};

typedef struct jblock jblock;
typedef struct {
    jblock *blocks;
} jdoc;

/* Parses text[0..len). Returns the root value, or NULL on error (err gets a reason).
 * All memory belongs to doc; call json_free(doc) when done (also after an error). */
jval *json_parse(jdoc *doc, const char *text, size_t len, char *err, size_t errlen);
void json_free(jdoc *doc);

/* Member lookup (first match), NULL when obj is not an object or the key is absent. */
const jval *json_get(const jval *obj, const char *key);
/* Helpers that accept NULL. */
int json_is_num(const jval *v);
int json_is_str(const jval *v);
int json_is_obj(const jval *v);
int json_is_arr(const jval *v);
double json_num(const jval *v, double def);
const char *json_str(const jval *v); /* NULL when not a string */

#endif
