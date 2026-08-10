#ifndef ERROR_H_
#define ERROR_H_

/*
 * Error reporting for a process that has no client to report to.
 *
 * weft note: this header used to declare set_default_response_param(),
 * send_error(), and send_service_unavailable_error(), all of them over
 * h2o_req_t, plus CHECK_YAJL_STATUS for the JSON body. src/utility.c
 * implemented them and nothing called it. A plane has no networking, so
 * there is no request to answer and no status code to send. What is left
 * writes to stderr.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define STANDARD_ERROR(msg) fprintf(stderr, "%s: %s\n", msg, strerror(errno))
#define LIBRARY_ERROR(lib, msg) fprintf(stderr, "%s: %s\n", lib, msg)
#define ERROR(msg) fprintf(stderr, "%s\n", msg)

#define IGNORE_FUNCTION_PARAMETER(x) (void)(x)

#define MKSTR(x) #x

#endif
