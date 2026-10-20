#include "yu_cap.h"

#include <errno.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t g_capture_mu = PTHREAD_MUTEX_INITIALIZER;

yu_system yu_system_new(void) {
  yu_system s;
  s.f_net = yu_alloc(sizeof *s.f_net); s.f_net->real = true;
  s.f_fs = yu_alloc(sizeof *s.f_fs); s.f_fs->real = true;
  s.f_env = yu_alloc(sizeof *s.f_env); s.f_env->real = true;
  s.f_clock = yu_alloc(sizeof *s.f_clock); s.f_clock->real = true;
  s.f_random = yu_alloc(sizeof *s.f_random); s.f_random->real = true; pthread_mutex_init(&s.f_random->mu, NULL);
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

// ---- Fs -------------------------------------------------------------------

static char *path_cstr(yu_str path) {
  char *p = yu_alloc((size_t)path.len + 1);
  memcpy(p, path.p, (size_t)path.len);
  p[path.len] = 0;
  return p;
}

static yu_str errno_str(void) {
  const char *m = strerror(errno);
  int64_t n = (int64_t)strlen(m);
  uint8_t *p = yu_alloc((size_t)n + 1);
  memcpy(p, m, (size_t)n);
  return (yu_str){p, n};
}

// Keeps a string in the main arena, which outlives every request (used by the in-memory Fs).
static yu_str keep(yu_str s) {
  yu_arena *prev = yu_arena_use(NULL);
  uint8_t *p = yu_alloc((size_t)s.len + 1);
  memcpy(p, s.p, (size_t)s.len);
  yu_arena_use(prev);
  return (yu_str){p, s.len};
}

int yu_fs_read(yu_fs *fs, yu_str path, yu_str *out, yu_str *msg) {
  if (!fs->real) {
    pthread_mutex_lock(&g_capture_mu);
    bool found = yu_map_get(fs->files, &path, out, sizeof(yu_str));
    pthread_mutex_unlock(&g_capture_mu);
    return found ? 0 : 1;
  }
  FILE *f = fopen(path_cstr(path), "rb");
  if (!f) { if (errno == ENOENT) return 1; *msg = errno_str(); return 2; }
  yu_sb b = {0};
  uint8_t buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) yu_sb_str(&b, (yu_str){buf, (int64_t)n});
  bool failed = ferror(f);
  fclose(f);
  if (failed) { *msg = errno_str(); return 2; }
  *out = yu_sb_finish(&b);
  return 0;
}

int yu_fs_write(yu_fs *fs, yu_str path, yu_str text, yu_str *msg) {
  if (!fs->real) {
    pthread_mutex_lock(&g_capture_mu);
    yu_arena *prev = yu_arena_use(NULL);
    yu_str k = keep(path), v = keep(text);
    yu_map_set(&fs->files, &k, &v, sizeof(yu_str));
    yu_arena_use(prev);
    pthread_mutex_unlock(&g_capture_mu);
    return 0;
  }
  FILE *f = fopen(path_cstr(path), "wb");
  if (!f) { if (errno == ENOENT) return 1; *msg = errno_str(); return 2; }
  bool ok = text.len == 0 || fwrite(text.p, 1, (size_t)text.len, f) == (size_t)text.len;
  if (fclose(f) != 0) ok = false;
  if (!ok) { *msg = errno_str(); return 2; }
  return 0;
}

yu_fs *yu_fs_memory(void) {
  yu_arena *prev = yu_arena_use(NULL);  // the file table outlives the request that made it
  yu_fs *fs = yu_alloc(sizeof *fs);
  fs->real = false;
  fs->files = yu_map_new(1, sizeof(yu_str));
  yu_arena_use(prev);
  return fs;
}

// ---- Random -----------------------------------------------------------------

static uint64_t splitmix(uint64_t *s) {
  uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static _Thread_local uint64_t g_rng = 0;  // the real generator: one stream per worker, seeded from the OS

static uint64_t next_u64(yu_random *r) {
  if (r->real) {
    if (g_rng == 0) {
      if (getentropy(&g_rng, sizeof g_rng) != 0 || g_rng == 0) g_rng = (uint64_t)yu_now_ms() ^ (uint64_t)(uintptr_t)&g_rng;
    }
    return splitmix(&g_rng);
  }
  pthread_mutex_lock(&r->mu);
  uint64_t v = splitmix(&r->state);
  pthread_mutex_unlock(&r->mu);
  return v;
}

int64_t yu_random_int(yu_random *r, int64_t max, const char *loc) {
  if (max <= 0) yu_fail("R-RAND-01", loc, "random.int needs max > 0, got %lld", (long long)max);
  uint64_t m = (uint64_t)max, lim = UINT64_MAX - UINT64_MAX % m;  // rejection sampling: no modulo bias
  uint64_t v;
  do v = next_u64(r); while (v >= lim);
  return (int64_t)(v % m);
}

yu_random *yu_random_seeded(int64_t seed) {
  yu_random *r = yu_alloc(sizeof *r);
  r->real = false;
  r->state = (uint64_t)seed;
  pthread_mutex_init(&r->mu, NULL);
  return r;
}

// ---- Tasks ------------------------------------------------------------------

typedef struct { yu_task_fn fn; void *env; const char *loc; } Task;
#define MAX_TASKS 64
static _Thread_local Task g_tasks[MAX_TASKS];
static _Thread_local int g_ntasks = 0;

static void run_task(Task t) {
  const char *prev_loc = yu_task_loc;
  yu_task_loc = t.loc;
  if (yu_catch_begin()) { t.fn(t.env); yu_catch_end(); }
  else {
    // the task's own error line is already printed; this one says which spawn it was
    yu_fail("R-TASK-01", t.loc, "spawned task failed");
  }
  yu_task_loc = prev_loc;
}

void yu_tasks_spawn(yu_tasks *t, yu_task_fn fn, void *env, const char *loc) {
  Task task = {fn, env, loc};
  if (t->inline_ || !yu_serving) {
    if (yu_catch_begin()) { run_task(task); yu_catch_end(); }  // R-TASK-01 ends the task, not the caller
    return;
  }
  if (g_ntasks == MAX_TASKS) yu_fail("R-TASK-02", loc, "more than %d tasks spawned by one request", MAX_TASKS);
  g_tasks[g_ntasks++] = task;
}

yu_tasks *yu_tasks_inline(void) {
  yu_tasks *t = yu_alloc(sizeof *t);
  t->inline_ = true;
  return t;
}

void yu_tasks_run(void) {
  for (int i = 0; i < g_ntasks; i++) {
    if (yu_catch_begin()) { run_task(g_tasks[i]); yu_catch_end(); }
  }
  g_ntasks = 0;
}
