// Runtime tests for hostile HTTP and JSON input. Built and run by tests/run_rt.sh with
// the address and undefined-behavior sanitizers; every case must be rejected cleanly.
#include <sys/socket.h>
#include <unistd.h>

#include "yu_rt.h"

static int failures = 0;

static void check(const char *name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) failures++;
}

// Feeds `request` to the parser through a socket pair and returns its verdict.
static int feed(const char *request, yu_http_request *req) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  size_t n = strlen(request);
  if (write(sv[1], request, n) != (ssize_t)n) return -1;
  close(sv[1]);  // the client is done sending
  int status = yu_http_read_request(sv[0], req);
  close(sv[0]);
  return status;
}

int main(void) {
  yu_http_request req;
  check("well-formed request is accepted", feed("POST /x HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi", &req) == 0 && req.f_body.len == 2);
  check("negative content-length is a bad request", feed("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n", &req) == 400);
  check("non-numeric content-length is a bad request", feed("POST /x HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n", &req) == 400);
  check("huge content-length is too large", feed("POST /x HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n", &req) == 413);
  check("short body is a bad request", feed("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nhi", &req) == 400);
  check("missing request line parts is a bad request", feed("GARBAGE\r\n\r\n", &req) == 400);

  // deeply nested JSON must fail as a syntax error, not overflow the stack
  size_t depth = 100000;
  char *deep = malloc(depth + 1);
  memset(deep, '[', depth);
  deep[depth] = 0;
  yu_jv *root = NULL;
  int64_t at = 0;
  bool parsed = yu_json_parse((yu_str){(const uint8_t *)deep, (int64_t)depth}, &root, &at);
  check("deeply nested json is rejected", !parsed);
  free(deep);
  // and reasonable nesting still works
  parsed = yu_json_parse(yu_str_lit("[[[[1]]]]", 9), &root, &at);
  check("moderate nesting is accepted", parsed);
  return failures ? 1 : 0;
}
