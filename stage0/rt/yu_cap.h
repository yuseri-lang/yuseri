// Yuseri Stage 0 runtime: capabilities (layer 2, ADR-0010). Every handle is a pointer to a
// runtime object; the generated code never looks inside. Test doubles live here too.
#ifndef YU_CAP_H
#define YU_CAP_H

#include "yu_rt.h"

typedef struct { bool real; } yu_net;
typedef struct { bool real; int64_t fixed_ms; } yu_clock;
typedef struct { bool real; yu_map vars; } yu_env;
typedef struct { bool capture; yu_list lines; } yu_log;
typedef struct { bool real; } yu_fs;       // placeholder until L4
typedef struct { bool real; } yu_random;   // placeholder until L4
typedef struct { bool inline_; } yu_tasks; // placeholder until L4

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

#endif
