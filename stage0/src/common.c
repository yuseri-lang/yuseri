#include "common.h"

#include <stdarg.h>

Str str_from(const char *p, int len) { return (Str){p, len}; }
Str str_cstr(const char *c) { return (Str){c, (int)strlen(c)}; }
bool str_eq(Str a, Str b) { return a.len == b.len && memcmp(a.p, b.p, (size_t)a.len) == 0; }
bool str_eq_c(Str a, const char *c) { return str_eq(a, str_cstr(c)); }

#define CHUNK_MIN (64 * 1024)

void *arena_alloc(Arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  ArenaChunk *c = a->head;
  if (!c || c->used + n > c->cap) {
    size_t cap = n > CHUNK_MIN ? n : CHUNK_MIN;
    c = malloc(sizeof(ArenaChunk) + cap);
    if (!c) {
      fprintf(stderr, "R-MEM-01 out of memory\n");
      exit(2);
    }
    c->next = a->head;
    c->used = 0;
    c->cap = cap;
    a->head = c;
  }
  void *p = c->data + c->used;
  c->used += n;
  memset(p, 0, n);
  return p;
}

char *arena_cstr(Arena *a, Str s) {
  char *p = arena_alloc(a, (size_t)s.len + 1);
  memcpy(p, s.p, (size_t)s.len);
  p[s.len] = 0;
  return p;
}

void arena_free(Arena *a) {
  ArenaChunk *c = a->head;
  while (c) {
    ArenaChunk *n = c->next;
    free(c);
    c = n;
  }
  a->head = NULL;
}

static void buf_reserve(Buf *b, int extra) {
  if (b->len + extra + 1 > b->cap) {
    int nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    b->p = realloc(b->p, (size_t)nc);
    if (!b->p) {
      fprintf(stderr, "R-MEM-01 out of memory\n");
      exit(2);
    }
    b->cap = nc;
  }
}

void buf_push(Buf *b, char c) {
  buf_reserve(b, 1);
  b->p[b->len++] = c;
  b->p[b->len] = 0;
}

void buf_write(Buf *b, const char *s, int n) {
  buf_reserve(b, n);
  memcpy(b->p + b->len, s, (size_t)n);
  b->len += n;
  b->p[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_write(b, s, (int)strlen(s)); }
void buf_str(Buf *b, Str s) { buf_write(b, s.p, s.len); }

void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n < (int)sizeof tmp) {
    buf_write(b, tmp, n);
    return;
  }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  buf_write(b, big, n);
  free(big);
}

void buf_free(Buf *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

typedef struct {
  const char *id;
  const char *hint;
} ErrorEntry;

static const ErrorEntry error_table[] = {
#define X(sym, id, hint) {id, hint},
#include "errors.def"
#undef X
};

const char *diag_hint(const char *id) {
  for (size_t i = 0; i < sizeof error_table / sizeof error_table[0]; i++)
    if (strcmp(error_table[i].id, id) == 0) return error_table[i].hint;
  return "see errors.md";
}

static int g_errors = 0;

void diag_error(const char *id, const char *file, int line, const char *fmt, ...) {
  g_errors++;
  fprintf(stderr, "%s %s:%d ", id, file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "; %s\n", diag_hint(id));
}

int diag_count(void) { return g_errors; }

bool read_file(const char *path, Str *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *p = malloc((size_t)n + 1);
  if (!p) {
    fclose(f);
    return false;
  }
  size_t got = fread(p, 1, (size_t)n, f);
  fclose(f);
  p[got] = 0;
  *out = (Str){p, (int)got};
  return true;
}
