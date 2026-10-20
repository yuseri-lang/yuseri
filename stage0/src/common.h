// Yuseri Stage 0: shared basics (arena, strings, vectors, diagnostics)
#ifndef YU_COMMON_H
#define YU_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *p;
  int len;
} Str;

#define STR_LIT(lit) ((Str){(lit), (int)sizeof(lit) - 1})
Str str_from(const char *p, int len);
Str str_cstr(const char *c);
bool str_eq(Str a, Str b);
bool str_eq_c(Str a, const char *c);

typedef struct ArenaChunk {
  struct ArenaChunk *next;
  size_t used, cap;
  char data[];
} ArenaChunk;

typedef struct {
  ArenaChunk *head;
} Arena;

void *arena_alloc(Arena *a, size_t n);
char *arena_cstr(Arena *a, Str s);
void arena_free(Arena *a);
#define NEW(a, T) ((T *)arena_alloc((a), sizeof(T)))

// Growable array stored in an arena. Usage: VEC(Expr *) items; VEC_PUSH(arena, &items, x);
#define VEC(T) \
  struct {     \
    T *items;  \
    int len;   \
    int cap;   \
  }
#define VEC_PUSH(a, v, x)                                                 \
  do {                                                                    \
    if ((v)->len == (v)->cap) {                                           \
      int nc_ = (v)->cap ? (v)->cap * 2 : 8;                              \
      void *ni_ = arena_alloc((a), (size_t)nc_ * sizeof(*(v)->items));    \
      if ((v)->len) memcpy(ni_, (v)->items, (size_t)(v)->len * sizeof(*(v)->items)); \
      (v)->items = ni_;                                                   \
      (v)->cap = nc_;                                                     \
    }                                                                     \
    (v)->items[(v)->len++] = (x);                                         \
  } while (0)

// Text buffer for output.
typedef struct {
  char *p;
  int len, cap;
} Buf;
void buf_push(Buf *b, char c);
void buf_write(Buf *b, const char *s, int n);
void buf_puts(Buf *b, const char *s);
void buf_str(Buf *b, Str s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_free(Buf *b);

// Diagnostics: one line per error, "<ID> <file>:<line> <message>; <hint>".
// With diag_buffer(true) errors are kept and printed sorted by line on diag_flush().
void diag_error(const char *id, const char *file, int line, const char *fmt, ...);
void diag_buffer(bool on);
void diag_flush(void);
int diag_count(void);
const char *diag_hint(const char *id);

bool read_file(const char *path, Str *out);

#endif
