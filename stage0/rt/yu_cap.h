// Yuseri Stage 0 runtime: capabilities (layer 2, ADR-0010). Every handle is a pointer to a
// runtime object; the generated code never looks inside. Test doubles live here too.
#ifndef YU_CAP_H
#define YU_CAP_H

#include "yu_rt.h"

typedef struct { bool real; } yu_net;
typedef struct { bool real; int64_t fixed_ms; } yu_clock;
typedef struct { bool real; yu_map vars; } yu_env;
typedef struct { bool capture; yu_list lines; } yu_log;
typedef struct { bool real; yu_map files; } yu_fs;                       // Fs.memory(): files is path -> text
typedef struct { bool real; uint64_t state; pthread_mutex_t mu; } yu_random;  // Random.seeded(n): state in the handle
typedef struct { bool inline_; } yu_tasks;

// Field order matches the injected `struct System` declaration.
typedef struct { yu_net *f_net; yu_fs *f_fs; yu_env *f_env; yu_clock *f_clock; yu_random *f_random; yu_log *f_log; yu_tasks *f_tasks; } yu_system;

yu_system yu_system_new(void);  // the real world; built once by the generated main

int64_t yu_clock_now(yu_clock *c);
yu_clock *yu_clock_fixed(int64_t ms);
bool yu_env_get(yu_env *e, yu_str name, yu_str *out);
yu_env *yu_env_of(yu_map vars);
void yu_log_write(yu_log *l, const char *level, yu_str msg);
yu_log *yu_log_capture(void);
yu_list yu_log_lines(yu_log *l);  // List<String>; empty for a real log

// Fs: text files. Both return 0 (ok), 1 (NotFound) or 2 (Io, with `msg` set).
int yu_fs_read(yu_fs *fs, yu_str path, yu_str *out, yu_str *msg);
int yu_fs_write(yu_fs *fs, yu_str path, yu_str text, yu_str *msg);
yu_fs *yu_fs_memory(void);

int64_t yu_random_int(yu_random *r, int64_t max, const char *loc);  // 0 <= n < max; R-RAND-01 when max <= 0
yu_random *yu_random_seeded(int64_t seed);

// Tasks: under http.serve a spawned task runs on the worker after the response is sent and
// before the request's arena is released (so the closure's environment is still alive);
// elsewhere, and with Tasks.inline(), it runs at once. A failure prints R-TASK-01 at `loc`.
typedef yu_unit (*yu_task_fn)(void *env);
void yu_tasks_spawn(yu_tasks *t, yu_task_fn fn, void *env, const char *loc);
yu_tasks *yu_tasks_inline(void);
void yu_tasks_run(void);    // runs and clears this worker's queue (called by the server)
void yu_tasks_clear(void);  // drops the queue without running it (the request failed)

#endif
