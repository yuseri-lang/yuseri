// Yuseri Stage 0 runtime: what generated C code calls.
// Every allocation comes from the current arena (one per worker thread, reset per request or
// per test); nothing is freed individually. The arena, the catch stack, the request id and the
// deadline are thread-local: a worker never sees another worker's request.
#ifndef YU_RT_H
#define YU_RT_H

#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char unused_; } yu_unit;
#define YU_UNIT ((yu_unit){0})

typedef struct { const uint8_t *p; int64_t len; } yu_str;
typedef struct { uint8_t *p; int64_t len; } yu_bytes;
typedef struct { void *p; int64_t len, cap; } yu_list;          // elements of esz bytes
typedef struct { yu_list keys, vals; int64_t *index; int64_t icap; int kkind; } yu_map;  // kkind: 0 Int, 1 String
typedef struct { void (*fn)(void); void *env; } yu_closure;     // cast to the typed shape at the call site

// ---- memory
void *yu_alloc(size_t n);
void yu_arena_reset(void);
typedef struct { void *chunk; size_t used; } yu_arena_mark;
yu_arena_mark yu_arena_mark_now(void);        // everything allocated after the mark...
void yu_arena_release(yu_arena_mark m);       // ...is freed here (request = arena)
typedef struct yu_arena yu_arena;             // a separate arena (used by Store)
yu_arena *yu_arena_new(void);
void yu_arena_free(yu_arena *a);
yu_arena *yu_arena_use(yu_arena *a);          // makes `a` current for yu_alloc; returns the previous one

// ---- Store<T>: the one place that outlives requests (layer 2, ADR-0009)
typedef struct {
  void *value;      // T, allocated in `arena`
  int64_t vsz;
  yu_arena *arena;  // owns `value` and everything it points to
  pthread_mutex_t mu;        // serializes get / set / update across workers
  yu_arena *pending, *prev_arena;  // a begun but not yet committed update (released on failure)
} yu_store;
yu_store *yu_store_new(int64_t vsz);
// Every access is bracketed by lock / unlock: get copies under the lock; set and update hold it
// from reading the current value to commit. lock fails with R-STORE-02 when this thread already
// holds it (update inside update). yu_fail releases whatever the failing thread holds.
void yu_store_lock(yu_store *s, const char *loc);
void yu_store_unlock(yu_store *s);
// update protocol: begin switches allocation to a fresh arena so the generated code can deep-copy
// the new value into it; commit installs that copy and frees the previous arena; abort discards it.
yu_arena *yu_store_begin(yu_store *s);
void yu_store_commit(yu_store *s, const void *copied, yu_arena *prev);
void yu_store_abort(yu_store *s, yu_arena *prev);

// ---- catching failures: yu_fail unwinds to the innermost active catch
#define yu_catch_begin() (yu_catch_push(), setjmp(*yu_catch_buf()) == 0)
void yu_catch_push(void);
jmp_buf *yu_catch_buf(void);
void yu_catch_end(void);

// ---- failures: prints "<ID> <loc> <message>; <hint>" then unwinds to the test harness or exits
void yu_fail(const char *id, const char *loc, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));

// ---- deadlines (layer 2, runtime.md 5): checked on allocation, at every loop iteration and on I/O
int64_t yu_now_ms(void);                        // monotonic milliseconds
void yu_deadline_set(int64_t at_ms, int64_t len_ms);  // 0 = no deadline
int64_t yu_deadline_remaining_ms(void);         // 0 when none or already passed
void yu_deadline_check(void);                   // R-TIME-01 when passed
extern _Thread_local uint32_t yu_ticks;
#define yu_tick() ((void)((++yu_ticks & 4095u) == 0 ? (yu_deadline_check(), 0) : 0))

// ---- integers (R-INT-01 overflow, R-INT-02 division by zero)
int64_t yu_add(int64_t a, int64_t b, const char *loc);
int64_t yu_sub(int64_t a, int64_t b, const char *loc);
int64_t yu_mul(int64_t a, int64_t b, const char *loc);
int64_t yu_div(int64_t a, int64_t b, const char *loc);
int64_t yu_mod(int64_t a, int64_t b, const char *loc);
int64_t yu_neg(int64_t a, const char *loc);

// ---- strings (immutable UTF-8; indices are byte offsets)
yu_str yu_str_lit(const char *s, int64_t n);
bool yu_str_eq(yu_str a, yu_str b);
int yu_str_cmp(yu_str a, yu_str b);
int64_t yu_str_len(yu_str s);
bool yu_str_is_empty(yu_str s);
yu_list yu_str_split(yu_str s, yu_str sep);
yu_str yu_str_trim(yu_str s);
bool yu_str_starts_with(yu_str s, yu_str p);
bool yu_str_ends_with(yu_str s, yu_str p);
bool yu_str_contains(yu_str s, yu_str p);
bool yu_str_find(yu_str s, yu_str p, int64_t *out);
yu_str yu_str_replace(yu_str s, yu_str from, yu_str to);
yu_str yu_str_to_upper(yu_str s);
yu_str yu_str_to_lower(yu_str s);
yu_str yu_str_slice(yu_str s, int64_t start, int64_t end, const char *loc);
yu_list yu_str_chars(yu_str s);
yu_bytes yu_str_bytes(yu_str s);
yu_str yu_str_join(yu_str sep, yu_list parts);
yu_str yu_str_repeat(yu_str s, int64_t n, const char *loc);
bool yu_int_parse(yu_str s, int64_t *out);

// ---- string builder (interpolation and value formatting)
typedef struct { uint8_t *p; int64_t len, cap; } yu_sb;
void yu_sb_str(yu_sb *b, yu_str s);
void yu_sb_cstr(yu_sb *b, const char *s);
void yu_sb_int(yu_sb *b, int64_t v);
void yu_sb_float(yu_sb *b, double v);
void yu_sb_bool(yu_sb *b, bool v);
void yu_sb_quoted(yu_sb *b, yu_str s);  // "..." with \" and \\ escaped (source form)
yu_str yu_sb_finish(yu_sb *b);

// ---- lists (value semantics are implemented by the generated code with yu_list_copy)
yu_list yu_list_new(int64_t esz);
void yu_list_push(yu_list *l, const void *elem, int64_t esz);
void *yu_list_at(yu_list l, int64_t i, int64_t esz, const char *loc);  // R-LIST-01
yu_list yu_list_copy(yu_list l, int64_t esz);

// ---- maps (insertion-ordered; keys are Int or String)
yu_map yu_map_new(int kkind, int64_t vsz);
void yu_map_set(yu_map *m, const void *key, const void *val, int64_t vsz);
bool yu_map_get(yu_map m, const void *key, void *out, int64_t vsz);
bool yu_map_has(yu_map m, const void *key);
void yu_map_remove(yu_map *m, const void *key, int64_t vsz);
int64_t yu_map_len(yu_map m);
yu_map yu_map_copy(yu_map m, int64_t vsz);
// entries as a list of structs laid out as { K key; V value; } with `voff` = offset of value
yu_list yu_map_entries(yu_map m, int64_t vsz, int64_t voff, int64_t esz);

// ---- error traces: the request id, then one "file:line" per Err and `?`
extern _Thread_local yu_str yu_request_id;
// Under http.serve every stderr line starts with the request id, error lines included, so one
// grep for the id shows everything the request did. Tests print error lines as they are.
extern _Thread_local bool yu_serving;
void yu_trace_add(yu_list *trace, const char *loc);

// ---- tests and assertions
void yu_assert(bool cond, const char *loc, const char *text);
void yu_assert_eq_fail(const char *loc, yu_str left, yu_str right) __attribute__((noreturn));
int yu_test_run(const char *name, void (*fn)(void));
int yu_test_summary(void);
void yu_init(void);

#include "yu_cap.h"
#include "yu_http.h"
#include "yu_json.h"

#endif
