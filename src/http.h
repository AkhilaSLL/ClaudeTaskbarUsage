#ifndef CTU_HTTP_H
#define CTU_HTTP_H

#include "engine.h"

/* GET https://api.anthropic.com/api/oauth/usage with the token (ctu_fetch_fn). */
void ctu_http_fetch(void *ctx, const char *token, ctu_http_result *res);

#endif
