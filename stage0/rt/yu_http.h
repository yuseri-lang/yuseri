// Yuseri Stage 0 runtime: a minimal HTTP/1.1 server, one request at a time.
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

struct yu_net_tag;
void yu_http_serve(void *net, int64_t port, yu_http_handler h);  // net: the Net capability
// Reads one request from a socket; exposed for tests. 0 = ok, else the status to answer.
int yu_http_read_request(int fd, yu_http_request *req);
yu_http_response yu_http_response_text(int64_t status, yu_str body);
yu_http_response yu_http_json(int64_t status, yu_str body);
yu_http_request yu_http_request_make(yu_str method, yu_str path, yu_str body);

#endif
