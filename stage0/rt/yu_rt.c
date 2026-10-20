#include "yu_rt.h"

#include <ctype.h>
#include <stdarg.h>
#include <time.h>

// ---- arena --------------------------------------------------------------

typedef struct Chunk { struct Chunk *next; size_t used, cap; char data[]; } Chunk;
struct yu_arena { Chunk *head; };
static yu_arena g_main_arena = {NULL};  // main's values; workers only read them
static _Thread_local yu_arena *g_cur = &g_main_arena;  // where yu_alloc allocates on this thread
#define CHUNK (256 * 1024)

void *yu_alloc(size_t n) {
  yu_tick();
  n = (n + 15) & ~(size_t)15;
  Chunk *c = g_cur->head;
  if (!c || c->used + n > c->cap) {
    size_t cap = n > CHUNK ? n : CHUNK;
    c = malloc(sizeof(Chunk) + cap);
    if (!c) { fprintf(stderr, "R-MEM-01 out of memory\n"); exit(2); }
    c->next = g_cur->head; c->used = 0; c->cap = cap;
    g_cur->head = c;
  }
  void *p = c->data + c->used;
  c->used += n;
  memset(p, 0, n);
  return p;
}

static void chunks_free(Chunk *c) { while (c) { Chunk *n = c->next; free(c); c = n; } }

void yu_arena_reset(void) { chunks_free(g_cur->head); g_cur->head = NULL; }

yu_arena_mark yu_arena_mark_now(void) {
  yu_arena_mark m = {g_cur->head, g_cur->head ? g_cur->head->used : 0};
  return m;
}

void yu_arena_release(yu_arena_mark m) {
  Chunk *c = g_cur->head;
  while (c && c != m.chunk) { Chunk *n = c->next; free(c); c = n; }
  g_cur->head = c;
  if (c) c->used = m.used;
}

yu_arena *yu_arena_new(void) { return calloc(1, sizeof(yu_arena)); }
void yu_arena_free(yu_arena *a) { if (!a) return; chunks_free(a->head); free(a); }
yu_arena *yu_arena_use(yu_arena *a) { yu_arena *prev = g_cur; g_cur = a ? a : &g_main_arena; return prev; }

// ---- Store ----------------------------------------------------------------

// Stores this thread is inside (lock held, or an update begun): released by yu_fail so a
// failing request never leaves a lock held or the thread allocating into a half-built arena.
typedef struct { yu_store *s; bool locked; } Held;
#define MAX_HELD 8
static _Thread_local Held g_held[MAX_HELD];
static _Thread_local int g_nheld = 0;

static Held *held_find(yu_store *s) {
  for (int i = 0; i < g_nheld; i++) if (g_held[i].s == s) return &g_held[i];
  return NULL;
}

static Held *held_push(yu_store *s, const char *loc) {
  Held *h = held_find(s);
  if (h) return h;
  if (g_nheld == MAX_HELD) yu_fail("R-STORE-02", loc, "too many nested Store operations");
  g_held[g_nheld] = (Held){s, false};
  return &g_held[g_nheld++];
}

static void held_drop(yu_store *s) {
  for (int i = 0; i < g_nheld; i++) {
    if (g_held[i].s != s) continue;
    memmove(&g_held[i], &g_held[i + 1], (size_t)(g_nheld - i - 1) * sizeof(Held));
    g_nheld--;
    return;
  }
}

yu_store *yu_store_new(int64_t vsz) {
  yu_store *s = calloc(1, sizeof *s);
  s->vsz = vsz;
  s->arena = yu_arena_new();
  pthread_mutex_init(&s->mu, NULL);
  yu_arena *prev = yu_arena_use(s->arena);
  s->value = yu_alloc((size_t)vsz);
  yu_arena_use(prev);
  return s;
}

void yu_store_lock(yu_store *s, const char *loc) {
  Held *h = held_find(s);
  if (h && h->locked) yu_fail("R-STORE-02", loc, "update inside an update of the same Store");
  pthread_mutex_lock(&s->mu);
  h = held_push(s, loc);
  h->locked = true;
}

void yu_store_unlock(yu_store *s) {
  Held *h = held_find(s);
  if (!h) return;
  if (s->pending) {  // begun but never committed: drop the half-built value
    yu_arena_use(s->prev_arena);
    yu_arena_free(s->pending);
    s->pending = NULL;
  }
  if (h->locked) pthread_mutex_unlock(&s->mu);
  held_drop(s);
}

static void release_all_held(void) {
  while (g_nheld > 0) yu_store_unlock(g_held[g_nheld - 1].s);
}

yu_arena *yu_store_begin(yu_store *s) {
  held_push(s, "Store");
  s->prev_arena = g_cur;
  s->pending = yu_arena_new();
  return yu_arena_use(s->pending);
}

void yu_store_commit(yu_store *s, const void *copied, yu_arena *prev) {
  yu_arena *fresh = yu_arena_use(prev);
  void *slot;
  {  // the slot itself lives in the fresh arena too
    yu_arena *p2 = yu_arena_use(fresh);
    slot = yu_alloc((size_t)s->vsz);
    yu_arena_use(p2);
  }
  memcpy(slot, copied, (size_t)s->vsz);
  yu_arena_free(s->arena);
  s->arena = fresh;
  s->value = slot;
  s->pending = NULL;
  Held *h = held_find(s);
  if (h && !h->locked) held_drop(s);
}

void yu_store_abort(yu_store *s, yu_arena *prev) {
  yu_arena *fresh = yu_arena_use(prev);
  yu_arena_free(fresh);
  s->pending = NULL;
  Held *h = held_find(s);
  if (h && !h->locked) held_drop(s);
}

// ---- deadlines ------------------------------------------------------------

_Thread_local uint32_t yu_ticks = 0;
static _Thread_local int64_t g_deadline_at = 0, g_deadline_len = 0;

int64_t yu_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void yu_deadline_set(int64_t at_ms, int64_t len_ms) { g_deadline_at = at_ms; g_deadline_len = len_ms; }
void yu_deadline_get(int64_t *at_ms, int64_t *len_ms) { *at_ms = g_deadline_at; *len_ms = g_deadline_len; }

int64_t yu_deadline_remaining_ms(void) {
  if (!g_deadline_at) return 0;
  int64_t left = g_deadline_at - yu_now_ms();
  return left > 0 ? left : 0;
}

void yu_deadline_check(void) {
  if (!g_deadline_at || yu_now_ms() <= g_deadline_at) return;
  int64_t len = g_deadline_len;
  g_deadline_at = 0;  // the failure path itself allocates; it must not fail again
  yu_fail("R-TIME-01", "std.http", "request exceeded its %lld ms deadline", (long long)len);
}

// ---- failures -------------------------------------------------------------

typedef struct { const char *id; const char *hint; } Hint;
static const Hint hints[] = {
#define X(sym, id, hint) {id, hint},
#include "../src/errors.def"
#undef X
};

static const char *hint_for(const char *id) {
  for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) if (strcmp(hints[i].id, id) == 0) return hints[i].hint;
  return "see errors.md";
}

static _Thread_local jmp_buf g_catch[8];
static _Thread_local int g_ncatch = 0;

void yu_catch_push(void) { if (g_ncatch < 8) g_ncatch++; }
jmp_buf *yu_catch_buf(void) { return &g_catch[g_ncatch - 1]; }
void yu_catch_end(void) { if (g_ncatch > 0) g_ncatch--; }

void yu_fail(const char *id, const char *loc, const char *fmt, ...) {
  char msg[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  // one fprintf call per line: lines from different workers never interleave
  if (yu_serving) fprintf(stderr, "%.*s %s %s %s; %s\n", (int)yu_request_id.len, (const char *)yu_request_id.p, id, loc, msg, hint_for(id));
  else fprintf(stderr, "%s %s %s; %s\n", id, loc, msg, hint_for(id));
  release_all_held();
  if (g_ncatch > 0) { int i = --g_ncatch; longjmp(g_catch[i], 1); }
  exit(1);
}

// ---- integers -------------------------------------------------------------

int64_t yu_add(int64_t a, int64_t b, const char *loc) { int64_t r; if (__builtin_add_overflow(a, b, &r)) yu_fail("R-INT-01", loc, "integer overflow in %lld + %lld", (long long)a, (long long)b); return r; }
int64_t yu_sub(int64_t a, int64_t b, const char *loc) { int64_t r; if (__builtin_sub_overflow(a, b, &r)) yu_fail("R-INT-01", loc, "integer overflow in %lld - %lld", (long long)a, (long long)b); return r; }
int64_t yu_mul(int64_t a, int64_t b, const char *loc) { int64_t r; if (__builtin_mul_overflow(a, b, &r)) yu_fail("R-INT-01", loc, "integer overflow in %lld * %lld", (long long)a, (long long)b); return r; }
int64_t yu_div(int64_t a, int64_t b, const char *loc) {
  if (b == 0) yu_fail("R-INT-02", loc, "division by zero");
  if (a == INT64_MIN && b == -1) yu_fail("R-INT-01", loc, "integer overflow in division");
  return a / b;
}
int64_t yu_mod(int64_t a, int64_t b, const char *loc) {
  if (b == 0) yu_fail("R-INT-02", loc, "division by zero");
  if (a == INT64_MIN && b == -1) return 0;
  return a % b;
}
int64_t yu_neg(int64_t a, const char *loc) { if (a == INT64_MIN) yu_fail("R-INT-01", loc, "integer overflow in negation"); return -a; }

// ---- strings --------------------------------------------------------------

yu_str yu_str_lit(const char *s, int64_t n) { return (yu_str){(const uint8_t *)s, n}; }
bool yu_str_eq(yu_str a, yu_str b) { return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0); }
int yu_str_cmp(yu_str a, yu_str b) {
  int64_t n = a.len < b.len ? a.len : b.len;
  int r = n ? memcmp(a.p, b.p, (size_t)n) : 0;
  if (r) return r;
  return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}
int64_t yu_str_len(yu_str s) { return s.len; }
bool yu_str_is_empty(yu_str s) { return s.len == 0; }

static yu_str str_dup(const uint8_t *p, int64_t n) {
  uint8_t *q = yu_alloc((size_t)n + 1);
  if (n) memcpy(q, p, (size_t)n);
  return (yu_str){q, n};
}

bool yu_str_find(yu_str s, yu_str p, int64_t *out) {
  if (p.len == 0) { *out = 0; return true; }
  for (int64_t i = 0; i + p.len <= s.len; i++)
    if (memcmp(s.p + i, p.p, (size_t)p.len) == 0) { *out = i; return true; }
  return false;
}

yu_list yu_str_split(yu_str s, yu_str sep) {
  yu_list out = yu_list_new(sizeof(yu_str));
  if (sep.len == 0) { yu_list_push(&out, &s, sizeof(yu_str)); return out; }
  int64_t start = 0;
  for (int64_t i = 0; i + sep.len <= s.len;) {
    if (memcmp(s.p + i, sep.p, (size_t)sep.len) == 0) {
      yu_str piece = {s.p + start, i - start};
      yu_list_push(&out, &piece, sizeof(yu_str));
      i += sep.len;
      start = i;
    } else i++;
  }
  yu_str last = {s.p + start, s.len - start};
  yu_list_push(&out, &last, sizeof(yu_str));
  return out;
}

static bool is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
yu_str yu_str_trim(yu_str s) {
  int64_t a = 0, b = s.len;
  while (a < b && is_ws(s.p[a])) a++;
  while (b > a && is_ws(s.p[b - 1])) b--;
  return (yu_str){s.p + a, b - a};
}
bool yu_str_starts_with(yu_str s, yu_str p) { return p.len <= s.len && (p.len == 0 || memcmp(s.p, p.p, (size_t)p.len) == 0); }
bool yu_str_ends_with(yu_str s, yu_str p) { return p.len <= s.len && (p.len == 0 || memcmp(s.p + s.len - p.len, p.p, (size_t)p.len) == 0); }
bool yu_str_contains(yu_str s, yu_str p) { int64_t i; return yu_str_find(s, p, &i); }

yu_str yu_str_replace(yu_str s, yu_str from, yu_str to) {
  if (from.len == 0) return s;
  yu_sb b = {0};
  int64_t i = 0;
  while (i < s.len) {
    if (i + from.len <= s.len && memcmp(s.p + i, from.p, (size_t)from.len) == 0) { yu_sb_str(&b, to); i += from.len; }
    else { uint8_t c = s.p[i++]; yu_sb_str(&b, (yu_str){&c, 1}); }
  }
  return yu_sb_finish(&b);
}

static yu_str map_ascii(yu_str s, int (*f)(int)) {
  yu_str d = str_dup(s.p, s.len);
  for (int64_t i = 0; i < d.len; i++) if (d.p[i] < 0x80) ((uint8_t *)d.p)[i] = (uint8_t)f(d.p[i]);
  return d;
}
yu_str yu_str_to_upper(yu_str s) { return map_ascii(s, toupper); }
yu_str yu_str_to_lower(yu_str s) { return map_ascii(s, tolower); }

static bool is_cont(uint8_t c) { return (c & 0xC0) == 0x80; }
yu_str yu_str_slice(yu_str s, int64_t start, int64_t end, const char *loc) {
  if (start < 0 || end > s.len || start > end) yu_fail("R-STR-01", loc, "slice %lld..%lld outside 0..%lld", (long long)start, (long long)end, (long long)s.len);
  if ((start < s.len && is_cont(s.p[start])) || (end < s.len && is_cont(s.p[end]))) yu_fail("R-STR-01", loc, "slice boundary inside a UTF-8 character");
  return (yu_str){s.p + start, end - start};
}

yu_list yu_str_chars(yu_str s) {
  yu_list out = yu_list_new(sizeof(yu_str));
  int64_t i = 0;
  while (i < s.len) {
    int64_t j = i + 1;
    while (j < s.len && is_cont(s.p[j])) j++;
    yu_str ch = {s.p + i, j - i};
    yu_list_push(&out, &ch, sizeof(yu_str));
    i = j;
  }
  return out;
}

yu_bytes yu_str_bytes(yu_str s) { yu_str d = str_dup(s.p, s.len); return (yu_bytes){(uint8_t *)d.p, d.len}; }

yu_str yu_str_join(yu_str sep, yu_list parts) {
  yu_sb b = {0};
  for (int64_t i = 0; i < parts.len; i++) {
    if (i) yu_sb_str(&b, sep);
    yu_sb_str(&b, ((yu_str *)parts.p)[i]);
  }
  return yu_sb_finish(&b);
}

yu_str yu_str_repeat(yu_str s, int64_t n, const char *loc) {
  if (n < 0) yu_fail("R-STR-01", loc, "repeat count %lld is negative", (long long)n);
  yu_sb b = {0};
  for (int64_t i = 0; i < n; i++) yu_sb_str(&b, s);
  return yu_sb_finish(&b);
}

bool yu_int_parse(yu_str s, int64_t *out) {
  int64_t i = 0, v = 0;
  bool neg = false;
  if (i < s.len && (s.p[i] == '-' || s.p[i] == '+')) { neg = s.p[i] == '-'; i++; }
  if (i >= s.len) return false;
  for (; i < s.len; i++) {
    if (s.p[i] < '0' || s.p[i] > '9') return false;
    if (__builtin_mul_overflow(v, 10, &v)) return false;
    if (__builtin_add_overflow(v, neg ? -(s.p[i] - '0') : (s.p[i] - '0'), &v)) return false;
  }
  *out = v;
  return true;
}

// ---- builder --------------------------------------------------------------

static void sb_reserve(yu_sb *b, int64_t extra) {
  if (b->len + extra > b->cap) {
    int64_t nc = b->cap ? b->cap * 2 : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *np = yu_alloc((size_t)nc);
    if (b->len) memcpy(np, b->p, (size_t)b->len);
    b->p = np; b->cap = nc;
  }
}
void yu_sb_str(yu_sb *b, yu_str s) { sb_reserve(b, s.len); if (s.len) memcpy(b->p + b->len, s.p, (size_t)s.len); b->len += s.len; }
void yu_sb_cstr(yu_sb *b, const char *s) { yu_sb_str(b, (yu_str){(const uint8_t *)s, (int64_t)strlen(s)}); }
void yu_sb_int(yu_sb *b, int64_t v) { char t[32]; int n = snprintf(t, sizeof t, "%lld", (long long)v); yu_sb_str(b, (yu_str){(const uint8_t *)t, n}); }
void yu_sb_float(yu_sb *b, double v) { char t[64]; int n = snprintf(t, sizeof t, "%.17g", v); if (!strchr(t, '.') && !strchr(t, 'e') && !strchr(t, 'n') && !strchr(t, 'i')) { t[n++] = '.'; t[n++] = '0'; } yu_sb_str(b, (yu_str){(const uint8_t *)t, n}); }
void yu_sb_bool(yu_sb *b, bool v) { yu_sb_cstr(b, v ? "true" : "false"); }
void yu_sb_quoted(yu_sb *b, yu_str s) {
  yu_sb_cstr(b, "\"");
  for (int64_t i = 0; i < s.len; i++) {
    uint8_t c = s.p[i];
    if (c == '"') yu_sb_cstr(b, "\\\"");
    else if (c == '\\') yu_sb_cstr(b, "\\\\");
    else if (c == '\n') yu_sb_cstr(b, "\\n");
    else yu_sb_str(b, (yu_str){&c, 1});
  }
  yu_sb_cstr(b, "\"");
}
yu_str yu_sb_finish(yu_sb *b) { yu_str s = {b->p ? b->p : (const uint8_t *)"", b->len}; return s; }

// ---- lists ----------------------------------------------------------------

yu_list yu_list_new(int64_t esz) { (void)esz; return (yu_list){NULL, 0, 0}; }

void yu_list_push(yu_list *l, const void *elem, int64_t esz) {
  if (l->len == l->cap) {
    int64_t nc = l->cap ? l->cap * 2 : 8;
    void *np = yu_alloc((size_t)(nc * esz));
    if (l->len) memcpy(np, l->p, (size_t)(l->len * esz));
    l->p = np; l->cap = nc;
  }
  memcpy((char *)l->p + l->len * esz, elem, (size_t)esz);
  l->len++;
}

void *yu_list_at(yu_list l, int64_t i, int64_t esz, const char *loc) {
  if (i < 0 || i >= l.len) yu_fail("R-LIST-01", loc, "index out of range: index %lld, len %lld", (long long)i, (long long)l.len);
  return (char *)l.p + i * esz;
}

yu_list yu_list_copy(yu_list l, int64_t esz) {
  yu_list n = {NULL, l.len, l.len};
  if (l.len) { n.p = yu_alloc((size_t)(l.len * esz)); memcpy(n.p, l.p, (size_t)(l.len * esz)); }
  return n;
}

// ---- maps -----------------------------------------------------------------

static uint64_t hash_bytes(const uint8_t *p, int64_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (int64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}
static uint64_t key_hash(const yu_map *m, const void *key) {
  if (m->kkind == 0) { int64_t k = *(const int64_t *)key; return hash_bytes((const uint8_t *)&k, 8); }
  const yu_str *s = key;
  return hash_bytes(s->p, s->len);
}
static bool key_eq(const yu_map *m, const void *a, const void *b) {
  if (m->kkind == 0) return *(const int64_t *)a == *(const int64_t *)b;
  return yu_str_eq(*(const yu_str *)a, *(const yu_str *)b);
}
static int64_t ksz(const yu_map *m) { return m->kkind == 0 ? (int64_t)sizeof(int64_t) : (int64_t)sizeof(yu_str); }

yu_map yu_map_new(int kkind, int64_t vsz) { (void)vsz; yu_map m = {0}; m.kkind = kkind; m.keys = yu_list_new(ksz(&m)); return m; }

static void rebuild_index(yu_map *m, int64_t cap) {
  m->icap = cap;
  m->index = yu_alloc((size_t)cap * sizeof(int64_t));
  for (int64_t i = 0; i < cap; i++) m->index[i] = -1;
  for (int64_t i = 0; i < m->keys.len; i++) {
    const void *k = (char *)m->keys.p + i * ksz(m);
    uint64_t h = key_hash(m, k) & (uint64_t)(cap - 1);
    while (m->index[h] != -1) h = (h + 1) & (uint64_t)(cap - 1);
    m->index[h] = i;
  }
}

static int64_t find_slot(const yu_map *m, const void *key) {
  if (m->icap == 0) return -1;
  uint64_t h = key_hash(m, key) & (uint64_t)(m->icap - 1);
  for (;;) {
    int64_t i = m->index[h];
    if (i == -1) return -1;
    if (key_eq(m, (char *)m->keys.p + i * ksz(m), key)) return i;
    h = (h + 1) & (uint64_t)(m->icap - 1);
  }
}

void yu_map_set(yu_map *m, const void *key, const void *val, int64_t vsz) {
  int64_t i = find_slot(m, key);
  if (i >= 0) { memcpy((char *)m->vals.p + i * vsz, val, (size_t)vsz); return; }
  yu_list_push(&m->keys, key, ksz(m));
  yu_list_push(&m->vals, val, vsz);
  if (m->keys.len * 10 > m->icap * 7) rebuild_index(m, m->icap ? m->icap * 2 : 16);
  else {
    uint64_t h = key_hash(m, key) & (uint64_t)(m->icap - 1);
    while (m->index[h] != -1) h = (h + 1) & (uint64_t)(m->icap - 1);
    m->index[h] = m->keys.len - 1;
  }
}

bool yu_map_get(yu_map m, const void *key, void *out, int64_t vsz) {
  int64_t i = find_slot(&m, key);
  if (i < 0) return false;
  memcpy(out, (char *)m.vals.p + i * vsz, (size_t)vsz);
  return true;
}

bool yu_map_has(yu_map m, const void *key) { return find_slot(&m, key) >= 0; }

void yu_map_remove(yu_map *m, const void *key, int64_t vsz) {
  int64_t i = find_slot(m, key);
  if (i < 0) return;
  int64_t k = ksz(m);
  memmove((char *)m->keys.p + i * k, (char *)m->keys.p + (i + 1) * k, (size_t)((m->keys.len - i - 1) * k));
  memmove((char *)m->vals.p + i * vsz, (char *)m->vals.p + (i + 1) * vsz, (size_t)((m->vals.len - i - 1) * vsz));
  m->keys.len--; m->vals.len--;
  rebuild_index(m, m->icap);
}

int64_t yu_map_len(yu_map m) { return m.keys.len; }

yu_map yu_map_copy(yu_map m, int64_t vsz) {
  yu_map n = m;
  n.keys = yu_list_copy(m.keys, ksz(&m));
  n.vals = yu_list_copy(m.vals, vsz);
  n.index = NULL; n.icap = 0;
  if (n.keys.len) rebuild_index(&n, m.icap);
  return n;
}

yu_list yu_map_entries(yu_map m, int64_t vsz, int64_t voff, int64_t esz) {
  yu_list out = yu_list_new(esz);
  int64_t k = ksz(&m);
  char *tmp = yu_alloc((size_t)esz);
  for (int64_t i = 0; i < m.keys.len; i++) {
    memset(tmp, 0, (size_t)esz);
    memcpy(tmp, (char *)m.keys.p + i * k, (size_t)k);
    memcpy(tmp + voff, (char *)m.vals.p + i * vsz, (size_t)vsz);
    yu_list_push(&out, tmp, esz);
  }
  return out;
}

// ---- traces ---------------------------------------------------------------

_Thread_local yu_str yu_request_id = {(const uint8_t *)"main", 4};
_Thread_local bool yu_serving = false;
_Thread_local const char *yu_task_loc = NULL;

// Copies the list first: enum values are copied by value, and two copies must never
// write into the same buffer (a trace must show only the path this value took).
void yu_trace_add(yu_list *trace, const char *loc) {
  *trace = yu_list_copy(*trace, sizeof(yu_str));
  if (trace->len == 0) {
    yu_list_push(trace, &yu_request_id, sizeof(yu_str));
    if (yu_task_loc) {  // inside a spawned task: the spawn site comes right after the request id
      yu_str s = {(const uint8_t *)yu_task_loc, (int64_t)strlen(yu_task_loc)};
      yu_list_push(trace, &s, sizeof(yu_str));
    }
  }
  yu_str l = {(const uint8_t *)loc, (int64_t)strlen(loc)};
  yu_list_push(trace, &l, sizeof(yu_str));
}

// ---- tests ----------------------------------------------------------------

static int g_tests = 0, g_failed = 0;

void yu_assert(bool cond, const char *loc, const char *text) {
  if (!cond) yu_fail("R-ASSERT-01", loc, "assertion failed: %s", text);
}

void yu_assert_eq_fail(const char *loc, yu_str left, yu_str right) {
  yu_fail("R-ASSERT-01", loc, "assert_eq failed: left %.*s, right %.*s", (int)left.len, (const char *)left.p, (int)right.len, (const char *)right.p);
}

int yu_test_run(const char *name, void (*fn)(void)) {
  g_tests++;
  yu_request_id = (yu_str){(const uint8_t *)name, (int64_t)strlen(name)};
  bool ok;
  if (yu_catch_begin()) { fn(); yu_catch_end(); ok = true; }
  else { ok = false; yu_arena_use(NULL); }
  if (ok) printf("ok   %s\n", name);
  else { printf("fail %s\n", name); g_failed++; }
  fflush(stdout);
  yu_arena_reset();
  return ok ? 0 : 1;
}

int yu_test_summary(void) {
  printf("%d test%s, %d failed\n", g_tests, g_tests == 1 ? "" : "s", g_failed);
  return g_failed ? 1 : 0;
}

void yu_init(void) {}
