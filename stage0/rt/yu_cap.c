#include "yu_cap.h"

#include <sys/time.h>

static pthread_mutex_t g_capture_mu = PTHREAD_MUTEX_INITIALIZER;

yu_system yu_system_new(void) {
  yu_system s;
  s.f_net = yu_alloc(sizeof *s.f_net); s.f_net->real = true;
  s.f_fs = yu_alloc(sizeof *s.f_fs); s.f_fs->real = true;
  s.f_env = yu_alloc(sizeof *s.f_env); s.f_env->real = true;
  s.f_clock = yu_alloc(sizeof *s.f_clock); s.f_clock->real = true;
  s.f_random = yu_alloc(sizeof *s.f_random); s.f_random->real = true;
  s.f_log = yu_alloc(sizeof *s.f_log); s.f_log->capture = false;
  s.f_tasks = yu_alloc(sizeof *s.f_tasks); s.f_tasks->inline_ = false;
  return s;
}

int64_t yu_clock_now(yu_clock *c) {
  if (!c->real) return c->fixed_ms;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

yu_clock *yu_clock_fixed(int64_t ms) {
  yu_clock *c = yu_alloc(sizeof *c);
  c->real = false;
  c->fixed_ms = ms;
  return c;
}

bool yu_env_get(yu_env *e, yu_str name, yu_str *out) {
  if (!e->real) return yu_map_get(e->vars, &name, out, sizeof(yu_str));
  char buf[256];
  if (name.len >= (int64_t)sizeof buf) return false;
  memcpy(buf, name.p, (size_t)name.len);
  buf[name.len] = 0;
  const char *v = getenv(buf);
  if (!v) return false;
  int64_t n = (int64_t)strlen(v);
  uint8_t *p = yu_alloc((size_t)n + 1);
  memcpy(p, v, (size_t)n);
  *out = (yu_str){p, n};
  return true;
}

yu_env *yu_env_of(yu_map vars) {
  yu_env *e = yu_alloc(sizeof *e);
  e->real = false;
  e->vars = yu_map_copy(vars, sizeof(yu_str));
  return e;
}

void yu_log_write(yu_log *l, const char *level, yu_str msg) {
  yu_sb b = {0};
  yu_sb_str(&b, yu_request_id);
  yu_sb_cstr(&b, " ");
  yu_sb_cstr(&b, level);
  yu_sb_cstr(&b, " ");
  yu_sb_str(&b, msg);
  yu_str line = yu_sb_finish(&b);
  if (l->capture) {
    // captured lines must outlive the request that wrote them: keep them in the main arena,
    // which workers otherwise never write to, so the push is serialized
    pthread_mutex_lock(&g_capture_mu);
    yu_arena *prev = yu_arena_use(NULL);
    yu_str kept = {NULL, line.len};
    uint8_t *p = yu_alloc((size_t)line.len + 1);
    memcpy(p, line.p, (size_t)line.len);
    kept.p = p;
    yu_list_push(&l->lines, &kept, sizeof(yu_str));
    yu_arena_use(prev);
    pthread_mutex_unlock(&g_capture_mu);
    return;
  }
  fprintf(stderr, "%.*s\n", (int)line.len, (const char *)line.p);  // one call per line
}

yu_log *yu_log_capture(void) {
  yu_log *l = yu_alloc(sizeof *l);
  l->capture = true;
  l->lines = yu_list_new(sizeof(yu_str));
  return l;
}

yu_list yu_log_lines(yu_log *l) {
  if (!l->capture) return yu_list_new(sizeof(yu_str));
  pthread_mutex_lock(&g_capture_mu);
  yu_list copy = yu_list_copy(l->lines, sizeof(yu_str));
  pthread_mutex_unlock(&g_capture_mu);
  return copy;
}
