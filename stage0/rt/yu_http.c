#include "yu_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static yu_map str_map(void) { return yu_map_new(1, sizeof(yu_str)); }

static void map_set(yu_map *m, yu_str k, yu_str v) { yu_map_set(m, &k, &v, sizeof(yu_str)); }

static yu_str dup_str(yu_str s) {
  uint8_t *p = yu_alloc((size_t)s.len + 1);
  if (s.len) memcpy(p, s.p, (size_t)s.len);
  return (yu_str){p, s.len};
}

static int hexv(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static yu_str percent_decode(yu_str s, bool plus_is_space) {
  yu_sb b = {0};
  for (int64_t i = 0; i < s.len; i++) {
    uint8_t c = s.p[i];
    if (c == '%' && i + 2 < s.len && hexv(s.p[i + 1]) >= 0 && hexv(s.p[i + 2]) >= 0) {
      uint8_t d = (uint8_t)(hexv(s.p[i + 1]) * 16 + hexv(s.p[i + 2]));
      yu_sb_str(&b, (yu_str){&d, 1});
      i += 2;
    } else if (c == '+' && plus_is_space) {
      yu_sb_cstr(&b, " ");
    } else yu_sb_str(&b, (yu_str){&c, 1});
  }
  return yu_sb_finish(&b);
}

static yu_str lower(yu_str s) {
  yu_str d = dup_str(s);
  for (int64_t i = 0; i < d.len; i++) if (d.p[i] >= 'A' && d.p[i] <= 'Z') ((uint8_t *)d.p)[i] = (uint8_t)(d.p[i] + 32);
  return d;
}

static yu_str upper(yu_str s) {
  yu_str d = dup_str(s);
  for (int64_t i = 0; i < d.len; i++) if (d.p[i] >= 'a' && d.p[i] <= 'z') ((uint8_t *)d.p)[i] = (uint8_t)(d.p[i] - 32);
  return d;
}

// Splits "path?query" into a decoded path and a query map.
static void split_target(yu_str target, yu_str *path, yu_map *query) {
  int64_t q = -1;
  for (int64_t i = 0; i < target.len; i++) if (target.p[i] == '?') { q = i; break; }
  yu_str raw_path = q < 0 ? target : (yu_str){target.p, q};
  *path = percent_decode(raw_path, false);
  *query = str_map();
  if (q < 0) return;
  yu_str qs = {target.p + q + 1, target.len - q - 1};
  yu_list pairs = yu_str_split(qs, yu_str_lit("&", 1));
  for (int64_t i = 0; i < pairs.len; i++) {
    yu_str pair = ((yu_str *)pairs.p)[i];
    if (pair.len == 0) continue;
    int64_t eq = -1;
    for (int64_t k = 0; k < pair.len; k++) if (pair.p[k] == '=') { eq = k; break; }
    yu_str k = eq < 0 ? pair : (yu_str){pair.p, eq};
    yu_str v = eq < 0 ? yu_str_lit("", 0) : (yu_str){pair.p + eq + 1, pair.len - eq - 1};
    map_set(query, percent_decode(k, true), percent_decode(v, true));
  }
}

yu_http_request yu_http_request_make(yu_str method, yu_str path, yu_str body) {
  yu_http_request r = {0};
  r.f_id = yu_str_lit("test", 4);
  r.f_method = upper(method);
  split_target(path, &r.f_path, &r.f_query);
  r.f_headers = str_map();
  r.f_body = body;
  return r;
}

yu_http_response yu_http_response_text(int64_t status, yu_str body) {
  yu_http_response r = {0};
  r.f_status = status;
  r.f_headers = str_map();
  map_set(&r.f_headers, yu_str_lit("content-type", 12), yu_str_lit("text/plain; charset=utf-8", 25));
  r.f_body = body;
  return r;
}

yu_http_response yu_http_json(int64_t status, yu_str body) {
  yu_http_response r = {0};
  r.f_status = status;
  r.f_headers = str_map();
  map_set(&r.f_headers, yu_str_lit("content-type", 12), yu_str_lit("application/json", 16));
  r.f_body = body;
  return r;
}

static const char *reason(int64_t status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "Status";
  }
}

static bool write_all(int fd, const uint8_t *p, int64_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, (size_t)n);
    if (w <= 0) return false;
    p += w;
    n -= w;
  }
  return true;
}

static void send_response(int fd, yu_http_response r) {
  yu_sb b = {0};
  char head[128];
  snprintf(head, sizeof head, "HTTP/1.1 %lld %s\r\n", (long long)r.f_status, reason(r.f_status));
  yu_sb_cstr(&b, head);
  for (int64_t i = 0; i < r.f_headers.keys.len; i++) {
    yu_sb_str(&b, ((yu_str *)r.f_headers.keys.p)[i]);
    yu_sb_cstr(&b, ": ");
    yu_sb_str(&b, ((yu_str *)r.f_headers.vals.p)[i]);
    yu_sb_cstr(&b, "\r\n");
  }
  snprintf(head, sizeof head, "content-length: %lld\r\nconnection: close\r\n\r\n", (long long)r.f_body.len);
  yu_sb_cstr(&b, head);
  yu_sb_str(&b, r.f_body);
  yu_str out = yu_sb_finish(&b);
  write_all(fd, out.p, out.len);
}

// Reads one request. Returns 0 when `req` is usable, or the status to answer with:
// 400 for a malformed or incomplete request, 413 for one that is too large.
int yu_http_read_request(int fd, yu_http_request *req) {
  yu_sb raw = {0};
  uint8_t buf[8192];
  int64_t header_end = -1;
  while (header_end < 0) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) return 400;
    yu_sb_str(&raw, (yu_str){buf, n});
    for (int64_t i = 3; i < raw.len; i++)
      if (raw.p[i - 3] == '\r' && raw.p[i - 2] == '\n' && raw.p[i - 1] == '\r' && raw.p[i] == '\n') { header_end = i + 1; break; }
    if (raw.len > YU_HTTP_MAX_HEADER) return 413;
  }
  yu_str head = {raw.p, header_end};
  // request line
  int64_t sp1 = -1, sp2 = -1, eol = -1;
  for (int64_t i = 0; i < head.len; i++) {
    if (head.p[i] == ' ' && sp1 < 0) sp1 = i;
    else if (head.p[i] == ' ' && sp2 < 0) sp2 = i;
    else if (head.p[i] == '\r') { eol = i; break; }
  }
  if (sp1 < 0 || sp2 < 0 || eol < 0) return 400;
  *req = (yu_http_request){0};
  req->f_method = upper((yu_str){head.p, sp1});
  split_target((yu_str){head.p + sp1 + 1, sp2 - sp1 - 1}, &req->f_path, &req->f_query);
  req->f_headers = str_map();
  int64_t content_length = 0;
  int64_t i = eol + 2;
  while (i < head.len) {
    int64_t j = i;
    while (j < head.len && head.p[j] != '\r') j++;
    yu_str line = {head.p + i, j - i};
    if (line.len == 0) break;
    int64_t colon = -1;
    for (int64_t k = 0; k < line.len; k++) if (line.p[k] == ':') { colon = k; break; }
    if (colon > 0) {
      yu_str name = lower((yu_str){line.p, colon});
      yu_str value = yu_str_trim((yu_str){line.p + colon + 1, line.len - colon - 1});
      map_set(&req->f_headers, name, dup_str(value));
      if (yu_str_eq(name, yu_str_lit("content-length", 14))) {
        // digits only, no sign: a negative or malformed length is a bad request
        if (value.len == 0 || value.len > 18) return 400;
        for (int64_t k = 0; k < value.len; k++) if (value.p[k] < '0' || value.p[k] > '9') return 400;
        int64_t v;
        if (!yu_int_parse(value, &v) || v < 0) return 400;
        if (v > YU_HTTP_MAX_BODY) return 413;
        content_length = v;
      }
    }
    i = j + 2;
  }
  while (raw.len - header_end < content_length) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    yu_sb_str(&raw, (yu_str){buf, n});
  }
  // the declared body must arrive in full; a short body is an incomplete request
  if (raw.len - header_end < content_length) return 400;
  req->f_body = dup_str((yu_str){raw.p + header_end, content_length});
  return 0;
}

// ---- the server: N workers, one request at a time each ----------------------------------

typedef struct { int listen_fd; yu_http_handler h; int64_t deadline_ms; } Server;
static _Atomic int64_t g_counter = 0;  // request ids are unique across workers

static void socket_timeout(int fd, int64_t ms) {
  if (ms <= 0) ms = 1;
  struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

// Handles one connection on the calling worker's arena. Everything from reading the request
// to the handler's return runs under the deadline and inside one catch: a failure anywhere
// answers 500 and the worker carries on. Exposed so tests can drive it through a socket pair.
void yu_http_handle_connection(yu_http_handler h, int fd, int64_t deadline_ms) {
  yu_arena *mine = yu_arena_use(NULL);  // the worker's arena, restored after a failure
  yu_arena_use(mine);
  yu_arena_mark mark = yu_arena_mark_now();
  yu_deadline_set(yu_now_ms() + deadline_ms, deadline_ms);
  socket_timeout(fd, deadline_ms);
  char idbuf[32];
  snprintf(idbuf, sizeof idbuf, "req-%lld", (long long)(++g_counter));
  yu_request_id = yu_str_lit(idbuf, (int64_t)strlen(idbuf));
  yu_http_request req;
  yu_http_response res;
  int status;  // 0 = handled, else the status to answer with
  if (yu_catch_begin()) {
    status = yu_http_read_request(fd, &req);
    if (status == 0) {
      req.f_id = dup_str(yu_request_id);
      res = h.fn(h.env, req);
    }
    yu_catch_end();
  } else {
    // a panic, runtime error or R-TIME-01: the error line is already printed
    yu_arena_use(mine);
    status = 500;
  }
  int64_t at, len;
  yu_deadline_get(&at, &len);
  yu_deadline_set(0, 0);  // the answer itself is not under the deadline
  if (status == 500) res = yu_http_response_text(500, yu_str_lit("internal error", 14));
  else if (status == 413) res = yu_http_response_text(413, yu_str_lit("request too large", 17));
  else if (status != 0) res = yu_http_response_text(status, yu_str_lit("bad request", 11));
  send_response(fd, res);
  close(fd);
  if (status == 0) {
    // spawned tasks run now: after the answer, under the same deadline, before the arena goes
    yu_deadline_set(at, len);
    yu_tasks_run();
    yu_deadline_set(0, 0);
  } else {
    // a failed request takes its tasks with it: their environments live in the arena that is
    // released below, so running them later would read freed memory (external review)
    yu_tasks_clear();
  }
  yu_request_id = yu_str_lit("main", 4);
  yu_arena_release(mark);
}

static void *worker_main(void *arg) {
  Server *sv = arg;
  yu_arena_use(yu_arena_new());
  yu_serving = true;
  for (;;) {
    int fd = accept(sv->listen_fd, NULL, NULL);
    if (fd < 0) continue;
    yu_http_handle_connection(sv->h, fd, sv->deadline_ms);
  }
  return NULL;
}

void yu_http_serve(void *net, int64_t port, yu_http_handler h, int64_t deadline_ms, int64_t workers) {
  (void)net;  // the capability is proof of permission; nothing to read from it yet
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) yu_fail("R-HTTP-01", "std.http", "cannot create a socket");
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) yu_fail("R-HTTP-01", "std.http", "cannot bind port %lld", (long long)port);
  if (listen(s, 128) < 0) yu_fail("R-HTTP-01", "std.http", "cannot listen on port %lld", (long long)port);
  if (workers <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    workers = n > 0 ? n : 1;
  }
  if (workers > 256) workers = 256;
  if (deadline_ms <= 0) deadline_ms = YU_HTTP_DEFAULT_DEADLINE_MS;
  Server *sv = calloc(1, sizeof *sv);
  sv->listen_fd = s;
  sv->h = h;
  sv->deadline_ms = deadline_ms;
  fprintf(stderr, "yuseri: listening on port %lld, %lld worker%s, %lld ms deadline\n", (long long)port, (long long)workers, workers == 1 ? "" : "s", (long long)deadline_ms);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);  // the same room as the main thread
  pthread_t *threads = calloc((size_t)workers, sizeof *threads);
  for (int64_t i = 0; i < workers; i++) {
    if (pthread_create(&threads[i], &attr, worker_main, sv) != 0) yu_fail("R-HTTP-01", "std.http", "cannot start worker %lld", (long long)i);
  }
  for (int64_t i = 0; i < workers; i++) pthread_join(threads[i], NULL);  // never returns
}
