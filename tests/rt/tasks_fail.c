// Runtime test for the external review of L1-L4: a request that spawns a task and then fails
// must not leave the task behind for the next request (its environment lives in the released
// arena). Two requests are driven through socket pairs; the sanitizers watch the second one.
#include <sys/socket.h>
#include <unistd.h>

#include "yu_rt.h"

static int failures = 0;

static void check(const char *name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) failures++;
}

static int g_ran = 0;
static yu_tasks g_tasks = {false};

typedef struct { yu_str note; } Env;

static yu_unit task(void *env) {
  Env *e = env;
  if (e->note.len == 4) g_ran++;  // reads the request's memory
  return YU_UNIT;
}

// Request 1 (path /fail): spawns a task whose environment is in the request's arena, then
// panics. Request 2 (path /ok): a normal request.
static yu_http_response handler(void *env, yu_http_request req) {
  (void)env;
  if (yu_str_eq(req.f_path, yu_str_lit("/fail", 5))) {
    Env *e = yu_alloc(sizeof *e);
    e->note = yu_str_lit("boom", 4);
    yu_tasks_spawn(&g_tasks, task, e, "tasks_fail.c:spawn");
    yu_fail("R-PANIC-01", "tasks_fail.c", "panic: after spawn");
  }
  return yu_http_response_text(200, yu_str_lit("ok", 2));
}

// Sends `request` through a socket pair and returns the status line's code.
static int drive(const char *request) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  size_t n = strlen(request);
  if (write(sv[1], request, n) != (ssize_t)n) return -1;
  shutdown(sv[1], SHUT_WR);
  yu_http_handler h = {handler, NULL};
  yu_http_handle_connection(h, sv[0], 1000);
  char buf[256] = {0};
  ssize_t got = read(sv[1], buf, sizeof buf - 1);
  close(sv[1]);
  if (got < 12) return -1;
  return atoi(buf + 9);
}

int main(void) {
  yu_arena_use(yu_arena_new());
  yu_serving = true;
  int s1 = drive("GET /fail HTTP/1.1\r\n\r\n");
  check("a request that panics after spawn answers 500", s1 == 500);
  check("its task did not run", g_ran == 0);
  // force the released memory to be reused
  for (int i = 0; i < 64; i++) { void *p = yu_alloc(4096); memset(p, 0xAB, 4096); }
  int s2 = drive("GET /ok HTTP/1.1\r\n\r\n");
  check("the next request is answered", s2 == 200);
  check("the failed request's task never ran", g_ran == 0);
  return failures ? 1 : 0;
}
