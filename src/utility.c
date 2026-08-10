#include <h2o.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <yajl/yajl_gen.h>

#include "utility.h"
#include "error.h"

uint32_t get_random_number(uint32_t max, unsigned int *seed)
{
    return (uint32_t)rand_r(seed) % max + 1;
}

void set_default_response_param(content_type_t type, size_t content_length, h2o_req_t *req)
{
    req->res.status = 200;
    req->res.reason = "OK";
    switch (type) {
    case JSON:
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
        break;
    case PLAIN:
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
        break;
    case HTML:
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/html; charset=utf-8"));
        break;
    }
    (void)content_length;
}

void send_error(int status_code, const char *error_msg, h2o_req_t *req)
{
    h2o_send_error_generic(req, status_code, "Error", error_msg, 0);
}

void send_service_unavailable_error(const char *error_msg, h2o_req_t *req)
{
    h2o_send_error_503(req, "Service Unavailable", error_msg, 0);
}

json_generator_t *get_json_generator(json_generator_t **generators, size_t *count)
{
    (void)generators;
    (void)count;
    json_generator_t *gen = malloc(sizeof(*gen));
    gen->gen = yajl_gen_alloc(NULL);
    gen->in_use = true;
    return gen;
}

void free_json_generator(json_generator_t *gen, json_generator_t **generators, size_t *count, size_t max)
{
    (void)generators;
    (void)count;
    (void)max;
    if (gen) {
        if (gen->gen)
            yajl_gen_free(gen->gen);
        free(gen);
    }
}

int send_json_gen(json_generator_t *gen, bool copy, h2o_req_t *req)
{
    const unsigned char *buf;
    size_t len;
    yajl_gen_get_buf(gen->gen, &buf, &len);

    req->res.status = 200;
    req->res.reason = "OK";
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));

    if (copy) {
        h2o_iovec_t body = h2o_strdup(&req->pool, (const char *)buf, len);
        h2o_send_inline(req, body.base, body.len);
    } else {
        h2o_send_inline(req, (const char *)buf, len);
    }
    return 0;
}

const char *get_query_param(const char *query, size_t query_len, const char *name, size_t name_len, size_t *out_len)
{
    const char *p = query;
    while (p < query + query_len) {
        size_t remaining = query_len - (size_t)(p - query);
        const char *next = memchr(p, '&', remaining);
        size_t pair_len = next ? (size_t)(next - p) : remaining;
        /* A real key=value match needs the key followed by '=', not
         * just a name_len-byte prefix match -- the old check matched
         * "session_id" against "session_idx=..." too, and returned a
         * pointer starting at the '=' itself (never skipped past it),
         * corrupting every downstream use of the value. */
        if (pair_len > name_len && p[name_len] == '=' && memcmp(p, name, name_len) == 0) {
            *out_len = pair_len - name_len - 1;
            return p + name_len + 1;
        }
        if (!next)
            break;
        p = next + 1;
    }
    return NULL;
}
