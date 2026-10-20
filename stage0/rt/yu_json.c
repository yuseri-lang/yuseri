#include "yu_json.h"

typedef struct { yu_str s; int64_t i; } P;

static void ws(P *p) { while (p->i < p->s.len && (p->s.p[p->i] == ' ' || p->s.p[p->i] == '\t' || p->s.p[p->i] == '\n' || p->s.p[p->i] == '\r')) p->i++; }
static bool at(P *p, char c) { return p->i < p->s.len && p->s.p[p->i] == c; }

static yu_jv *node(int kind) { yu_jv *v = yu_alloc(sizeof *v); v->kind = kind; return v; }

static bool parse_value(P *p, yu_jv **out);

static int hexval(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void push_utf8(yu_sb *b, uint32_t cp) {
  uint8_t t[4];
  int n = 0;
  if (cp < 0x80) { t[0] = (uint8_t)cp; n = 1; }
  else if (cp < 0x800) { t[0] = (uint8_t)(0xC0 | (cp >> 6)); t[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) { t[0] = (uint8_t)(0xE0 | (cp >> 12)); t[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3; }
  else { t[0] = (uint8_t)(0xF0 | (cp >> 18)); t[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (uint8_t)(0x80 | (cp & 0x3F)); n = 4; }
  yu_sb_str(b, (yu_str){t, n});
}

static bool parse_string(P *p, yu_str *out) {
  if (!at(p, '"')) return false;
  p->i++;
  yu_sb b = {0};
  while (p->i < p->s.len) {
    uint8_t c = p->s.p[p->i];
    if (c == '"') { p->i++; *out = yu_sb_finish(&b); return true; }
    if (c < 0x20) return false;
    if (c == '\\') {
      p->i++;
      if (p->i >= p->s.len) return false;
      uint8_t e = p->s.p[p->i++];
      switch (e) {
        case '"': yu_sb_cstr(&b, "\""); break;
        case '\\': yu_sb_cstr(&b, "\\"); break;
        case '/': yu_sb_cstr(&b, "/"); break;
        case 'b': yu_sb_cstr(&b, "\b"); break;
        case 'f': yu_sb_cstr(&b, "\f"); break;
        case 'n': yu_sb_cstr(&b, "\n"); break;
        case 'r': yu_sb_cstr(&b, "\r"); break;
        case 't': yu_sb_cstr(&b, "\t"); break;
        case 'u': {
          if (p->i + 4 > p->s.len) return false;
          uint32_t cp = 0;
          for (int k = 0; k < 4; k++) { int h = hexval(p->s.p[p->i + k]); if (h < 0) return false; cp = cp * 16 + (uint32_t)h; }
          p->i += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->s.len && p->s.p[p->i] == '\\' && p->s.p[p->i + 1] == 'u') {
            uint32_t lo = 0;
            for (int k = 0; k < 4; k++) { int h = hexval(p->s.p[p->i + 2 + k]); if (h < 0) return false; lo = lo * 16 + (uint32_t)h; }
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p->i += 6; }
          }
          push_utf8(&b, cp);
          break;
        }
        default: return false;
      }
      continue;
    }
    yu_sb_str(&b, (yu_str){&c, 1});
    p->i++;
  }
  return false;
}

static bool parse_number(P *p, yu_jv **out) {
  int64_t start = p->i;
  if (at(p, '-')) p->i++;
  if (!(p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9')) return false;
  if (p->s.p[p->i] == '0') p->i++;
  else while (p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9') p->i++;
  if (at(p, '.')) { p->i++; if (!(p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9')) return false; while (p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9') p->i++; }
  if (at(p, 'e') || at(p, 'E')) {
    p->i++;
    if (at(p, '+') || at(p, '-')) p->i++;
    if (!(p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9')) return false;
    while (p->i < p->s.len && p->s.p[p->i] >= '0' && p->s.p[p->i] <= '9') p->i++;
  }
  yu_jv *v = node(YU_JV_NUMBER);
  v->text = (yu_str){p->s.p + start, p->i - start};
  *out = v;
  return true;
}

static bool lit(P *p, const char *word) {
  int64_t n = (int64_t)strlen(word);
  if (p->i + n > p->s.len || memcmp(p->s.p + p->i, word, (size_t)n) != 0) return false;
  p->i += n;
  return true;
}

static bool parse_value(P *p, yu_jv **out) {
  ws(p);
  if (p->i >= p->s.len) return false;
  uint8_t c = p->s.p[p->i];
  if (c == '{') {
    p->i++;
    yu_jv *v = node(YU_JV_OBJECT);
    v->items = yu_list_new(sizeof(yu_jkv));
    ws(p);
    if (at(p, '}')) { p->i++; *out = v; return true; }
    for (;;) {
      ws(p);
      yu_jkv kv;
      if (!parse_string(p, &kv.key)) return false;
      ws(p);
      if (!at(p, ':')) return false;
      p->i++;
      if (!parse_value(p, &kv.val)) return false;
      yu_list_push(&v->items, &kv, sizeof kv);
      ws(p);
      if (at(p, ',')) { p->i++; continue; }
      if (at(p, '}')) { p->i++; *out = v; return true; }
      return false;
    }
  }
  if (c == '[') {
    p->i++;
    yu_jv *v = node(YU_JV_ARRAY);
    v->items = yu_list_new(sizeof(yu_jv *));
    ws(p);
    if (at(p, ']')) { p->i++; *out = v; return true; }
    for (;;) {
      yu_jv *e;
      if (!parse_value(p, &e)) return false;
      yu_list_push(&v->items, &e, sizeof e);
      ws(p);
      if (at(p, ',')) { p->i++; continue; }
      if (at(p, ']')) { p->i++; *out = v; return true; }
      return false;
    }
  }
  if (c == '"') { yu_jv *v = node(YU_JV_STRING); if (!parse_string(p, &v->text)) return false; *out = v; return true; }
  if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, out);
  if (lit(p, "true")) { yu_jv *v = node(YU_JV_BOOL); v->b = true; *out = v; return true; }
  if (lit(p, "false")) { yu_jv *v = node(YU_JV_BOOL); v->b = false; *out = v; return true; }
  if (lit(p, "null")) { *out = node(YU_JV_NULL); return true; }
  return false;
}

bool yu_json_parse(yu_str text, yu_jv **out, int64_t *err_at) {
  P p = {text, 0};
  if (!parse_value(&p, out)) { *err_at = p.i; return false; }
  ws(&p);
  if (p.i != text.len) { *err_at = p.i; return false; }
  return true;
}

yu_jv *yu_jv_get(yu_jv *obj, yu_str key) {
  if (!obj || obj->kind != YU_JV_OBJECT) return NULL;
  for (int64_t i = 0; i < obj->items.len; i++) {
    yu_jkv *kv = &((yu_jkv *)obj->items.p)[i];
    if (yu_str_eq(kv->key, key)) return kv->val;
  }
  return NULL;
}

bool yu_jv_int(yu_jv *v, int64_t *out) {
  if (!v || v->kind != YU_JV_NUMBER) return false;
  for (int64_t i = 0; i < v->text.len; i++) { uint8_t c = v->text.p[i]; if (c == '.' || c == 'e' || c == 'E') return false; }
  return yu_int_parse(v->text, out);
}

bool yu_jv_float(yu_jv *v, double *out) {
  if (!v || v->kind != YU_JV_NUMBER) return false;
  char buf[64];
  if (v->text.len >= (int64_t)sizeof buf) return false;
  memcpy(buf, v->text.p, (size_t)v->text.len);
  buf[v->text.len] = 0;
  *out = strtod(buf, NULL);
  return true;
}

void yu_json_quote(yu_sb *b, yu_str s) {
  yu_sb_cstr(b, "\"");
  for (int64_t i = 0; i < s.len; i++) {
    uint8_t c = s.p[i];
    if (c == '"') yu_sb_cstr(b, "\\\"");
    else if (c == '\\') yu_sb_cstr(b, "\\\\");
    else if (c == '\n') yu_sb_cstr(b, "\\n");
    else if (c == '\r') yu_sb_cstr(b, "\\r");
    else if (c == '\t') yu_sb_cstr(b, "\\t");
    else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); yu_sb_cstr(b, t); }
    else yu_sb_str(b, (yu_str){&c, 1});
  }
  yu_sb_cstr(b, "\"");
}

const char *yu_jv_kind_name(yu_jv *v) {
  if (!v) return "missing";
  switch (v->kind) {
    case YU_JV_NULL: return "null";
    case YU_JV_BOOL: return "bool";
    case YU_JV_NUMBER: return "number";
    case YU_JV_STRING: return "string";
    case YU_JV_ARRAY: return "array";
    default: return "object";
  }
}

void yu_json_path_push(yu_sb *path, const char *seg, int64_t index) {
  if (seg) { if (path->len) yu_sb_cstr(path, "."); yu_sb_cstr(path, seg); }
  else { char t[32]; snprintf(t, sizeof t, "[%lld]", (long long)index); yu_sb_cstr(path, t); }
}
