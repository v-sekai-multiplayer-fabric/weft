#ifndef ERROR_H_
#define ERROR_H_

#include <h2o.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "global_data.h"

#define INTERNAL_SERVER_ERROR 500
#define BAD_GATEWAY 502
#define GATEWAY_TIMEOUT 504
#define REQ_ERROR "Request error"
#define DB_ERROR "Database error"
#define DB_TIMEOUT_ERROR "Database timeout"

#define STANDARD_ERROR(msg) fprintf(stderr, "%s: %s\n", msg, strerror(errno))
#define LIBRARY_ERROR(lib, msg) fprintf(stderr, "%s: %s\n", lib, msg)
#define ERROR(msg) fprintf(stderr, "%s\n", msg)
#define CHECK_YAJL_STATUS(fn, ...) do { yajl_gen_status _s = fn(__VA_ARGS__); if (_s != yajl_gen_status_ok) goto error_yajl; } while(0)

#define IGNORE_FUNCTION_PARAMETER(x) (void)(x)

#define MKSTR(x) #x

typedef enum { DONE = 1 } result_return_t;

typedef enum {
    PLAIN,
    JSON,
    HTML
} content_type_t;

void set_default_response_param(content_type_t type, size_t content_length, h2o_req_t *req);
void send_error(int status_code, const char *error_msg, h2o_req_t *req);
void send_service_unavailable_error(const char *error_msg, h2o_req_t *req);

#endif
