// Yuseri Stage 0 runtime: a minimal HTTP/1.1 server. N worker threads (one per CPU unless
// `workers` says otherwise) share the listening socket; each handles one request at a time on
// its own arena, under a deadline (runtime.md 5).
// The field layout of the two structs is what the compiler emits for std.http's
// Request and Response (same field order, f_ prefixed names).
#ifndef YU_HTTP_H
#define YU_HTTP_H

#include "yu_rt.h"

typedef struct { yu_str f_id, f_method, f_path; yu_map f_query, f_headers; yu_str f_body; } yu_http_request;
typedef struct { int64_t f_status; yu_map f_headers; yu_str f_body; } yu_http_response;
typedef yu_http_response (*yu_http_handler_fn)(void *env, yu_http_request req);
typedef struct { yu_http_handler_fn fn; void *env; } yu_http_handler;

// Limits: headers and body larger than these are answered with 413.
#define YU_HTTP_MAX_HEADER (64 * 1024)
#define YU_HTTP_MAX_BODY (1024 * 1024)

// net: the Net capability. deadline_ms: per request. workers: thread count, 0 = number of CPUs.
void yu_http_serve(void *net, int64_t port, yu_http_handler h, int64_t deadline_ms, int64_t workers);
#define YU_HTTP_DEFAULT_DEADLINE_MS 30000
// Reads one request from a socket; exposed for tests. 0 = ok, else the status to answer.
int yu_http_read_request(int fd, yu_http_request *req);
yu_http_response yu_http_response_text(int64_t status, yu_str body);
yu_http_response yu_http_json(int64_t status, yu_str body);
yu_http_request yu_http_request_make(yu_str method, yu_str path, yu_str body);

#endif
