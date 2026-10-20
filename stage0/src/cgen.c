#include "cgen.h"

#include <stdarg.h>

#include "types.h"

// ---- generator state ---------------------------------------------------

typedef struct {
  Arena *a;
  Module *m;
  const char *file;
  Buf types;    // typedefs, in dependency order
  Buf protos;   // prototypes of helpers, user functions, anonymous functions
  Buf helpers;  // eq / fmt / copy helper bodies
  Buf code;     // function bodies
  StrVec emitted_types, eq_done, fmt_done, copy_done, thunk_done;
  int tmp;      // temporary counter
  int indent;
  Buf *out;     // current body buffer
  Ty *fn_ret;   // return type of the function being generated (for `?`)
  const char *fn_cname;
  DeclVec structs, enums, consts, fns;
  // Yuseri bindings in scope, innermost last; each gets a unique C name
  struct { Str name; const char *cname; } *binds;
  int nbinds, cbinds;
  // generic instance being generated: parameter names and their concrete types
  StrVec *sub_names;
  TyVec *sub_binds;
  StrVec instances;  // generated generic instances (C names)
} G;

// Applies the current generic substitution to a type from the checked AST.
static Ty *sub(G *g, Ty *t) {
  if (!t || !g->sub_names) return t;
  return ty_subst(g->a, t, g->sub_names, g->sub_binds);
}

static char *sfmt(G *g, const char *fmt, ...);

static int scope_push(G *g) { return g->nbinds; }
static void scope_pop(G *g, int mark) { g->nbinds = mark; }

// Declares a new binding and returns its C name. Call after the initializer is generated.
static const char *bind_new(G *g, Str name) {
  if (g->nbinds == g->cbinds) {
    g->cbinds = g->cbinds ? g->cbinds * 2 : 32;
    void *nb = arena_alloc(g->a, (size_t)g->cbinds * sizeof(*g->binds));
    if (g->nbinds) memcpy(nb, g->binds, (size_t)g->nbinds * sizeof(*g->binds));
    g->binds = nb;
  }
  const char *cname = sfmt(g, "v_%s_%d", arena_cstr(g->a, name), ++g->tmp);
  g->binds[g->nbinds].name = name;
  g->binds[g->nbinds].cname = cname;
  g->nbinds++;
  return cname;
}

static const char *bind_ref(G *g, Str name) {
  for (int i = g->nbinds - 1; i >= 0; i--) if (str_eq(g->binds[i].name, name)) return g->binds[i].cname;
  return sfmt(g, "v_%s", arena_cstr(g->a, name));  // not reached for checked code
}

static void gen_stmts(G *g, Block *b);
static const char *gen_expr(G *g, Expr *e);
static const char *gen_place(G *g, Expr *e);
static void gen_block_into(G *g, Block *b, const char *target, Ty *ty);
static const char *ctype(G *g, Ty *t);
static const char *eq_fn(G *g, Ty *t);
static const char *fmt_fn(G *g, Ty *t);
static const char *copy_fn(G *g, Ty *t);

static char *cs(G *g, Str s) { return arena_cstr(g->a, s); }

static char *sfmt(G *g, const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) return arena_cstr(g->a, str_from(tmp, n));
  char *big = arena_alloc(g->a, (size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return big;
}

static void ind(G *g) { for (int i = 0; i < g->indent; i++) buf_puts(g->out, "  "); }

static void emit(G *g, const char *fmt, ...) {
  char tmp[4096];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  ind(g);
  if (n < (int)sizeof tmp) buf_write(g->out, tmp, n);
  else {
    char *big = malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_write(g->out, big, n);
    free(big);
  }
  buf_push(g->out, '\n');
}

static const char *newtmp(G *g) { return sfmt(g, "_t%d", ++g->tmp); }

static const char *loc(G *g, int line) { return sfmt(g, "\"%s:%d\"", g->file, line); }

static bool seen(StrVec *v, const char *name) {
  for (int i = 0; i < v->len; i++) if (str_eq_c(v->items[i], name)) return true;
  return false;
}
static void mark(G *g, StrVec *v, const char *name) { VEC_PUSH(g->a, v, str_cstr(name)); }

static Decl *find_decl(DeclVec *v, Str name) {
  for (int i = 0; i < v->len; i++) if (str_eq(v->items[i]->name, name)) return v->items[i];
  return NULL;
}

// ---- types -------------------------------------------------------------

static const char *mangle(G *g, Ty *t) {
  t = sub(g, t);
  switch (t->kind) {
    case TY_INT: return "Int";
    case TY_FLOAT: return "Float";
    case TY_BOOL: return "Bool";
    case TY_STRING: return "String";
    case TY_BYTES: return "Bytes";
    case TY_UNIT: case TY_NEVER: case TY_ERR: return "Unit";
    case TY_LIST: return sfmt(g, "List_%s_", mangle(g, t->a));
    case TY_MAP: return sfmt(g, "Map_%s_%s_", mangle(g, t->a), mangle(g, t->b));
    case TY_OPTION: return sfmt(g, "Opt_%s_", mangle(g, t->a));
    case TY_RESULT: return sfmt(g, "Res_%s_%s_", mangle(g, t->a), mangle(g, t->b));
    case TY_ENTRY: return sfmt(g, "Ent_%s_%s_", mangle(g, t->a), mangle(g, t->b));
    case TY_STORE: return sfmt(g, "Store_%s_", mangle(g, t->a));
    case TY_CAP: return sfmt(g, "Cap_%s", cs(g, t->name));
    case TY_STRUCT: case TY_ENUM: {
      Buf b = {0};
      buf_printf(&b, "S_%s", cs(g, t->name));
      for (int i = 0; i < t->args.len; i++) buf_printf(&b, "_%s", mangle(g, t->args.items[i]));
      if (t->args.len) buf_puts(&b, "_");
      const char *r = arena_cstr(g->a, str_from(b.p, b.len));
      buf_free(&b);
      return r;
    }
    case TY_FN: {
      Buf b = {0};
      buf_puts(&b, "Fn");
      for (int i = 0; i < t->params.len; i++) buf_printf(&b, "_%s", mangle(g, t->params.items[i]));
      buf_printf(&b, "_R_%s_", mangle(g, t->ret));
      const char *r = arena_cstr(g->a, str_from(b.p, b.len));
      buf_free(&b);
      return r;
    }
    case TY_PARAM: return sfmt(g, "P_%s", cs(g, t->name));
  }
  return "Unit";
}

// Field type of a (possibly generic) struct instance.
static Ty *field_ty(G *g, Ty *st, Param *p) {
  return ty_subst(g->a, p->ty, &st->decl->generics, &st->args);
}

static void emit_type(G *g, Ty *t);

static void emit_typedef_once(G *g, const char *name, const char *body) {
  if (seen(&g->emitted_types, name)) return;
  mark(g, &g->emitted_types, name);
  buf_puts(&g->types, body);
  buf_puts(&g->types, "\n");
}

static void emit_type(G *g, Ty *t) {
  t = sub(g, t);
  switch (t->kind) {
    case TY_LIST: emit_type(g, t->a); return;
    case TY_MAP: emit_type(g, t->a); emit_type(g, t->b); return;
    case TY_OPTION: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      emit_type(g, t->a);
      emit_typedef_once(g, n, sfmt(g, "typedef struct { bool some; %s val; } yu_%s;", ctype(g, t->a), n));
      return;
    }
    case TY_RESULT: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      emit_type(g, t->a);
      emit_type(g, t->b);
      emit_typedef_once(g, n, sfmt(g, "typedef struct { bool ok; %s val; %s err; } yu_%s;", ctype(g, t->a), ctype(g, t->b), n));
      return;
    }
    case TY_ENTRY: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      emit_type(g, t->a);
      emit_type(g, t->b);
      emit_typedef_once(g, n, sfmt(g, "typedef struct { %s key; %s value; } yu_%s;", ctype(g, t->a), ctype(g, t->b), n));
      return;
    }
    case TY_FN: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      for (int i = 0; i < t->params.len; i++) emit_type(g, t->params.items[i]);
      emit_type(g, t->ret);
      Buf b = {0};
      buf_printf(&b, "typedef %s (*yu_%s_p)(void *env", ctype(g, t->ret), n);
      for (int i = 0; i < t->params.len; i++) buf_printf(&b, ", %s", ctype(g, t->params.items[i]));
      buf_printf(&b, ");\ntypedef struct { yu_%s_p fn; void *env; } yu_%s;", n, n);
      emit_typedef_once(g, n, b.p);
      buf_free(&b);
      return;
    }
    case TY_STRUCT: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      if (!t->decl) return;
      mark(g, &g->emitted_types, n);  // mark first: a field may mention this struct inside a List
      if (t->decl->from_std && str_eq_c(t->name, "Request")) { buf_puts(&g->types, "typedef yu_http_request yu_S_Request;\n"); return; }
      if (t->decl->from_std && str_eq_c(t->name, "Response")) { buf_puts(&g->types, "typedef yu_http_response yu_S_Response;\n"); return; }
      if (t->decl->from_std && str_eq_c(t->name, "System")) { buf_puts(&g->types, "typedef yu_system yu_S_System;\n"); return; }
      Buf b = {0};
      buf_printf(&b, "typedef struct {");
      for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        emit_type(g, ft);
        buf_printf(&b, " %s f_%s;", ctype(g, ft), cs(g, t->decl->params.items[i].name));
      }
      if (t->decl->params.len == 0) buf_puts(&b, " char unused_;");
      buf_printf(&b, " } yu_%s;", n);
      buf_puts(&g->types, b.p);
      buf_puts(&g->types, "\n");
      buf_free(&b);
      return;
    }
    case TY_ENUM: {
      const char *n = mangle(g, t);
      if (seen(&g->emitted_types, n)) return;
      mark(g, &g->emitted_types, n);
      if (!t->decl) {  // builtin enum (ParseError): tag and trace only
        buf_printf(&g->types, "typedef struct { int tag; yu_list trace_; } yu_%s;\n", n);
        return;
      }
      Buf b = {0};
      bool any_fields = false;
      for (int i = 0; i < t->decl->variants.len; i++) {
        Variant *v = &t->decl->variants.items[i];
        buf_printf(&b, "enum { yu_%s_%s = %d };\n", n, cs(g, v->name), i);
        if (v->fields.len) any_fields = true;
      }
      buf_printf(&b, "typedef struct { int tag; yu_list trace_;");
      if (any_fields) {
        buf_puts(&b, " union {");
        for (int i = 0; i < t->decl->variants.len; i++) {
          Variant *v = &t->decl->variants.items[i];
          if (!v->fields.len) continue;
          buf_puts(&b, " struct {");
          for (int k = 0; k < v->fields.len; k++) {
            Ty *ft = ty_subst(g->a, v->fields.items[k].ty, &t->decl->generics, &t->args);
            emit_type(g, ft);
            buf_printf(&b, " %s f_%s;", ctype(g, ft), cs(g, v->fields.items[k].name));
          }
          buf_printf(&b, " } v_%s;", cs(g, v->name));
        }
        buf_puts(&b, " } u;");
      }
      buf_printf(&b, " } yu_%s;", n);
      buf_puts(&g->types, b.p);
      buf_puts(&g->types, "\n");
      buf_free(&b);
      return;
    }
    default: return;
  }
}

static const char *ctype(G *g, Ty *t) {
  t = sub(g, t);
  switch (t->kind) {
    case TY_INT: return "int64_t";
    case TY_FLOAT: return "double";
    case TY_BOOL: return "bool";
    case TY_STRING: return "yu_str";
    case TY_BYTES: return "yu_bytes";
    case TY_UNIT: case TY_NEVER: case TY_ERR: case TY_PARAM: return "yu_unit";
    case TY_LIST: return "yu_list";
    case TY_MAP: return "yu_map";
    case TY_STORE: emit_type(g, t->a); return "yu_store *";
    case TY_CAP: {
      if (str_eq_c(t->name, "Net")) return "yu_net *";
      if (str_eq_c(t->name, "Clock")) return "yu_clock *";
      if (str_eq_c(t->name, "Env")) return "yu_env *";
      if (str_eq_c(t->name, "Log")) return "yu_log *";
      if (str_eq_c(t->name, "Fs")) return "yu_fs *";
      if (str_eq_c(t->name, "Random")) return "yu_random *";
      return "yu_tasks *";
    }
    default: emit_type(g, t); return sfmt(g, "yu_%s", mangle(g, t));
  }
}

static bool needs_copy_raw(Ty *t);
static bool needs_copy(G *g, Ty *t) { return needs_copy_raw(sub(g, t)); }

static bool needs_copy_raw(Ty *t) {
  switch (t->kind) {
    case TY_LIST: case TY_MAP: return true;
    case TY_OPTION: return needs_copy_raw(t->a);
    case TY_RESULT: case TY_ENTRY: return needs_copy_raw(t->a) || needs_copy_raw(t->b);
    case TY_STRUCT:
      if (!t->decl) return false;
      for (int i = 0; i < t->decl->params.len; i++) if (needs_copy_raw(t->decl->params.items[i].ty)) return true;
      for (int i = 0; i < t->args.len; i++) if (needs_copy_raw(t->args.items[i])) return true;
      return false;
    case TY_ENUM:
      if (!t->decl) return false;
      for (int i = 0; i < t->decl->variants.len; i++)
        for (int k = 0; k < t->decl->variants.items[i].fields.len; k++)
          if (needs_copy_raw(t->decl->variants.items[i].fields.items[k].ty)) return true;
      for (int i = 0; i < t->args.len; i++) if (needs_copy_raw(t->args.items[i])) return true;
      return false;
    default: return false;
  }
}

static int map_kkind(Ty *k) { return k->kind == TY_INT ? 0 : 1; }

// ---- helpers: eq, fmt, copy --------------------------------------------

static const char *eq_fn(G *g, Ty *t) {
  t = sub(g, t);
  const char *n = sfmt(g, "yu_eq_%s", mangle(g, t));
  if (seen(&g->eq_done, n)) return n;
  mark(g, &g->eq_done, n);
  const char *ct = ctype(g, t);
  buf_printf(&g->protos, "static bool %s(%s a, %s b);\n", n, ct, ct);
  Buf b = {0};
  buf_printf(&b, "static bool %s(%s a, %s b) {\n", n, ct, ct);
  switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: buf_puts(&b, "  return a == b;\n"); break;
    case TY_STRING: buf_puts(&b, "  return yu_str_eq(a, b);\n"); break;
    case TY_BYTES: buf_puts(&b, "  return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);\n"); break;
    case TY_UNIT: case TY_NEVER: case TY_ERR: case TY_PARAM: case TY_FN: case TY_STORE: case TY_CAP: buf_puts(&b, "  (void)a; (void)b; return true;\n"); break;
    case TY_LIST: {
      const char *et = ctype(g, t->a);
      buf_printf(&b, "  if (a.len != b.len) return false;\n  for (int64_t i = 0; i < a.len; i++) if (!%s(((%s *)a.p)[i], ((%s *)b.p)[i])) return false;\n  return true;\n", eq_fn(g, t->a), et, et);
      break;
    }
    case TY_MAP: {
      const char *kt = ctype(g, t->a), *vt = ctype(g, t->b);
      buf_printf(&b, "  if (a.keys.len != b.keys.len) return false;\n  for (int64_t i = 0; i < a.keys.len; i++) {\n    %s v;\n    if (!yu_map_get(b, (char *)a.keys.p + i * sizeof(%s), &v, sizeof(%s))) return false;\n    if (!%s(((%s *)a.vals.p)[i], v)) return false;\n  }\n  return true;\n", vt, kt, vt, eq_fn(g, t->b), vt);
      break;
    }
    case TY_OPTION: buf_printf(&b, "  if (a.some != b.some) return false;\n  return !a.some || %s(a.val, b.val);\n", eq_fn(g, t->a)); break;
    case TY_RESULT: buf_printf(&b, "  if (a.ok != b.ok) return false;\n  return a.ok ? %s(a.val, b.val) : %s(a.err, b.err);\n", eq_fn(g, t->a), eq_fn(g, t->b)); break;
    case TY_ENTRY: buf_printf(&b, "  return %s(a.key, b.key) && %s(a.value, b.value);\n", eq_fn(g, t->a), eq_fn(g, t->b)); break;
    case TY_STRUCT: {
      if (!t->decl || t->decl->params.len == 0) { buf_puts(&b, "  (void)a; (void)b; return true;\n"); break; }
      buf_puts(&b, "  return true");
      for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        const char *fn = cs(g, t->decl->params.items[i].name);
        buf_printf(&b, " && %s(a.f_%s, b.f_%s)", eq_fn(g, ft), fn, fn);
      }
      buf_puts(&b, ";\n");
      break;
    }
    case TY_ENUM: {
      buf_puts(&b, "  if (a.tag != b.tag) return false;\n");
      if (t->decl) {
        for (int i = 0; i < t->decl->variants.len; i++) {
          Variant *v = &t->decl->variants.items[i];
          if (!v->fields.len) continue;
          buf_printf(&b, "  if (a.tag == %d) return true", i);
          for (int k = 0; k < v->fields.len; k++) {
            Ty *ft = ty_subst(g->a, v->fields.items[k].ty, &t->decl->generics, &t->args);
            const char *vn = cs(g, v->name), *fn = cs(g, v->fields.items[k].name);
            buf_printf(&b, " && %s(a.u.v_%s.f_%s, b.u.v_%s.f_%s)", eq_fn(g, ft), vn, fn, vn, fn);
          }
          buf_puts(&b, ";\n");
        }
      }
      buf_puts(&b, "  return true;\n");
      break;
    }
  }
  buf_puts(&b, "}\n");
  buf_puts(&g->helpers, b.p);
  buf_free(&b);
  return n;
}

// Appends the source-literal form of a value to a builder.
static const char *fmt_fn(G *g, Ty *t) {
  t = sub(g, t);
  const char *n = sfmt(g, "yu_fmt_%s", mangle(g, t));
  if (seen(&g->fmt_done, n)) return n;
  mark(g, &g->fmt_done, n);
  const char *ct = ctype(g, t);
  buf_printf(&g->protos, "static void %s(yu_sb *b, %s v);\n", n, ct);
  Buf b = {0};
  buf_printf(&b, "static void %s(yu_sb *b, %s v) {\n", n, ct);
  switch (t->kind) {
    case TY_INT: buf_puts(&b, "  yu_sb_int(b, v);\n"); break;
    case TY_FLOAT: buf_puts(&b, "  yu_sb_float(b, v);\n"); break;
    case TY_BOOL: buf_puts(&b, "  yu_sb_bool(b, v);\n"); break;
    case TY_STRING: buf_puts(&b, "  yu_sb_quoted(b, v);\n"); break;
    case TY_BYTES: buf_puts(&b, "  yu_sb_cstr(b, \"Bytes(\"); yu_sb_int(b, v.len); yu_sb_cstr(b, \")\");\n"); break;
    case TY_UNIT: case TY_NEVER: case TY_ERR: case TY_PARAM: buf_puts(&b, "  (void)v; yu_sb_cstr(b, \"Unit\");\n"); break;
    case TY_FN: buf_puts(&b, "  (void)v; yu_sb_cstr(b, \"fn\");\n"); break;
    case TY_STORE: buf_puts(&b, "  (void)v; yu_sb_cstr(b, \"Store\");\n"); break;
    case TY_CAP: buf_printf(&b, "  (void)v; yu_sb_cstr(b, \"%s\");\n", cs(g, t->name)); break;
    case TY_LIST:
      buf_printf(&b, "  yu_sb_cstr(b, \"[\");\n  for (int64_t i = 0; i < v.len; i++) { if (i) yu_sb_cstr(b, \", \"); %s(b, ((%s *)v.p)[i]); }\n  yu_sb_cstr(b, \"]\");\n", fmt_fn(g, t->a), ctype(g, t->a));
      break;
    case TY_MAP:
      buf_printf(&b, "  yu_sb_cstr(b, \"Map { \");\n  for (int64_t i = 0; i < v.keys.len; i++) { if (i) yu_sb_cstr(b, \", \"); %s(b, ((%s *)v.keys.p)[i]); yu_sb_cstr(b, \": \"); %s(b, ((%s *)v.vals.p)[i]); }\n  yu_sb_cstr(b, v.keys.len ? \" }\" : \"}\");\n", fmt_fn(g, t->a), ctype(g, t->a), fmt_fn(g, t->b), ctype(g, t->b));
      break;
    case TY_OPTION: buf_printf(&b, "  if (v.some) { yu_sb_cstr(b, \"Some(\"); %s(b, v.val); yu_sb_cstr(b, \")\"); } else yu_sb_cstr(b, \"None\");\n", fmt_fn(g, t->a)); break;
    case TY_RESULT: buf_printf(&b, "  if (v.ok) { yu_sb_cstr(b, \"Ok(\"); %s(b, v.val); } else { yu_sb_cstr(b, \"Err(\"); %s(b, v.err); }\n  yu_sb_cstr(b, \")\");\n", fmt_fn(g, t->a), fmt_fn(g, t->b)); break;
    case TY_ENTRY: buf_printf(&b, "  yu_sb_cstr(b, \"Entry { key: \"); %s(b, v.key); yu_sb_cstr(b, \", value: \"); %s(b, v.value); yu_sb_cstr(b, \" }\");\n", fmt_fn(g, t->a), fmt_fn(g, t->b)); break;
    case TY_STRUCT: {
      buf_printf(&b, "  yu_sb_cstr(b, \"%s { \");\n", cs(g, t->name));
      if (t->decl) for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        const char *fn = cs(g, t->decl->params.items[i].name);
        buf_printf(&b, "  yu_sb_cstr(b, \"%s%s: \"); %s(b, v.f_%s);\n", i ? ", " : "", fn, fmt_fn(g, ft), fn);
      }
      buf_printf(&b, "  yu_sb_cstr(b, \"%s}\");\n", t->decl && t->decl->params.len ? " " : "");
      break;
    }
    case TY_ENUM: {
      if (!t->decl) { buf_printf(&b, "  (void)v; yu_sb_cstr(b, \"%s\");\n", cs(g, t->name)); break; }
      for (int i = 0; i < t->decl->variants.len; i++) {
        Variant *v = &t->decl->variants.items[i];
        buf_printf(&b, "  if (v.tag == %d) { yu_sb_cstr(b, \"%s.%s\");", i, cs(g, t->name), cs(g, v->name));
        if (v->fields.len) {
          buf_puts(&b, " yu_sb_cstr(b, \"(\");");
          for (int k = 0; k < v->fields.len; k++) {
            Ty *ft = ty_subst(g->a, v->fields.items[k].ty, &t->decl->generics, &t->args);
            const char *fn = cs(g, v->fields.items[k].name);
            buf_printf(&b, " yu_sb_cstr(b, \"%s%s: \"); %s(b, v.u.v_%s.f_%s);", k ? ", " : "", fn, fmt_fn(g, ft), cs(g, v->name), fn);
          }
          buf_puts(&b, " yu_sb_cstr(b, \")\");");
        }
        buf_puts(&b, " return; }\n");
      }
      break;
    }
  }
  buf_puts(&b, "}\n");
  buf_puts(&g->helpers, b.p);
  buf_free(&b);
  return n;
}

// Deep copy for values that contain a List or Map (value semantics).
static const char *copy_fn(G *g, Ty *t) {
  t = sub(g, t);
  const char *n = sfmt(g, "yu_copy_%s", mangle(g, t));
  if (seen(&g->copy_done, n)) return n;
  mark(g, &g->copy_done, n);
  const char *ct = ctype(g, t);
  buf_printf(&g->protos, "static %s %s(%s v);\n", ct, n, ct);
  Buf b = {0};
  buf_printf(&b, "static %s %s(%s v) {\n", ct, n, ct);
  switch (t->kind) {
    case TY_LIST: {
      const char *et = ctype(g, t->a);
      buf_printf(&b, "  yu_list r = yu_list_copy(v, sizeof(%s));\n", et);
      if (needs_copy(g, t->a)) buf_printf(&b, "  for (int64_t i = 0; i < r.len; i++) ((%s *)r.p)[i] = %s(((%s *)r.p)[i]);\n", et, copy_fn(g, t->a), et);
      buf_puts(&b, "  return r;\n");
      break;
    }
    case TY_MAP: {
      const char *vt = ctype(g, t->b);
      buf_printf(&b, "  yu_map r = yu_map_copy(v, sizeof(%s));\n", vt);
      if (needs_copy(g, t->b)) buf_printf(&b, "  for (int64_t i = 0; i < r.vals.len; i++) ((%s *)r.vals.p)[i] = %s(((%s *)r.vals.p)[i]);\n", vt, copy_fn(g, t->b), vt);
      buf_puts(&b, "  return r;\n");
      break;
    }
    case TY_OPTION: buf_printf(&b, "  if (v.some) v.val = %s(v.val);\n  return v;\n", copy_fn(g, t->a)); break;
    case TY_RESULT:
      if (needs_copy(g, t->a)) buf_printf(&b, "  if (v.ok) v.val = %s(v.val);\n", copy_fn(g, t->a));
      if (needs_copy(g, t->b)) buf_printf(&b, "  if (!v.ok) v.err = %s(v.err);\n", copy_fn(g, t->b));
      buf_puts(&b, "  return v;\n");
      break;
    case TY_ENTRY:
      if (needs_copy(g, t->a)) buf_printf(&b, "  v.key = %s(v.key);\n", copy_fn(g, t->a));
      if (needs_copy(g, t->b)) buf_printf(&b, "  v.value = %s(v.value);\n", copy_fn(g, t->b));
      buf_puts(&b, "  return v;\n");
      break;
    case TY_STRUCT:
      for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        if (needs_copy(g, ft)) buf_printf(&b, "  v.f_%s = %s(v.f_%s);\n", cs(g, t->decl->params.items[i].name), copy_fn(g, ft), cs(g, t->decl->params.items[i].name));
      }
      buf_puts(&b, "  return v;\n");
      break;
    case TY_ENUM:
      for (int i = 0; i < t->decl->variants.len; i++) {
        Variant *v = &t->decl->variants.items[i];
        for (int k = 0; k < v->fields.len; k++) {
          Ty *ft = ty_subst(g->a, v->fields.items[k].ty, &t->decl->generics, &t->args);
          if (needs_copy(g, ft)) buf_printf(&b, "  if (v.tag == %d) v.u.v_%s.f_%s = %s(v.u.v_%s.f_%s);\n", i, cs(g, v->name), cs(g, v->fields.items[k].name), copy_fn(g, ft), cs(g, v->name), cs(g, v->fields.items[k].name));
        }
      }
      buf_puts(&b, "  return v;\n");
      break;
    default: buf_puts(&b, "  return v;\n"); break;
  }
  buf_puts(&b, "}\n");
  buf_puts(&g->helpers, b.p);
  buf_free(&b);
  return n;
}

// ---- string literals ---------------------------------------------------

// Converts a raw Yuseri string segment (escapes kept) into a C string literal body.
static const char *c_string(G *g, Str raw, int64_t *out_len) {
  Buf b = {0};
  int64_t len = 0;
  for (int i = 0; i < raw.len; i++) {
    unsigned char c = (unsigned char)raw.p[i];
    if (c == '\\' && i + 1 < raw.len) {
      char n = raw.p[++i];
      if (n == 'n') c = '\n';
      else if (n == '"') c = '"';
      else if (n == '\\') c = '\\';
      else if (n == '{') c = '{';
      else if (n == '}') c = '}';
      else if (n == 't') c = '\t';
      else c = (unsigned char)n;
    }
    len++;
    if (c == '"') buf_puts(&b, "\\\"");
    else if (c == '\\') buf_puts(&b, "\\\\");
    else if (c == '\n') buf_puts(&b, "\\n");
    else if (c < 0x20 || c >= 0x7f) buf_printf(&b, "\\%03o", c);
    else buf_push(&b, (char)c);
  }
  *out_len = len;
  const char *r = arena_cstr(g->a, str_from(b.p ? b.p : "", b.len));
  buf_free(&b);
  return r;
}

static const char *str_lit(G *g, Str raw) {
  int64_t n;
  const char *body = c_string(g, raw, &n);
  return sfmt(g, "yu_str_lit(\"%s\", %lld)", body, (long long)n);
}

// ---- expressions -------------------------------------------------------

// True for expressions that build a brand-new value (nothing else can share its buffers).
static bool is_fresh(Expr *e) {
  return e->kind == E_LIST || e->kind == E_TYPECTOR || e->kind == E_STRING || (e->kind == E_STRUCT && !e->base);
}

// Reads a value for storing somewhere new. Values that contain a List or Map are deep-copied
// unless the expression is a fresh constructor: this keeps the value semantics of core.md.
static const char *gen_value(G *g, Expr *e) {
  const char *v = gen_expr(g, e);
  if (e->ty && needs_copy(g, e->ty) && !is_fresh(e)) return sfmt(g, "%s(%s)", copy_fn(g, e->ty), v);
  return v;
}

// Evaluates `e` into a fresh temporary and returns its name (so it can be used twice).
static const char *gen_temp(G *g, Expr *e) {
  const char *v = gen_expr(g, e);
  const char *t = newtmp(g);
  emit(g, "%s %s = %s;", ctype(g, e->ty), t, v);
  return t;
}

static const char *fn_cname(G *g, Str name) { return sfmt(g, "y_%s", cs(g, name)); }
static const char *method_cname(G *g, Str type, Str name) { return sfmt(g, "y_%s_%s", cs(g, type), cs(g, name)); }
static const char *var_cname(G *g, Str name) { return sfmt(g, "v_%s", cs(g, name)); }

// A named function used as a value: a thunk with the closure calling convention.
static const char *fn_thunk(G *g, Decl *d) {
  const char *n = sfmt(g, "y_%s__thunk", cs(g, d->name));
  if (seen(&g->thunk_done, n)) return n;
  mark(g, &g->thunk_done, n);
  Ty *ft = d->ty;
  Buf b = {0};
  buf_printf(&b, "static %s %s(void *env", ctype(g, ft->ret), n);
  for (int i = 0; i < ft->params.len; i++) buf_printf(&b, ", %s a%d", ctype(g, ft->params.items[i]), i);
  buf_puts(&b, ")");
  buf_printf(&g->protos, "%s;\n", b.p);
  buf_printf(&g->helpers, "%s { (void)env; return %s(", b.p, fn_cname(g, d->name));
  for (int i = 0; i < ft->params.len; i++) buf_printf(&g->helpers, "%sa%d", i ? ", " : "", i);
  buf_puts(&g->helpers, "); }\n");
  buf_free(&b);
  return n;
}

static const char *gen_call(G *g, Expr *e);
static const char *gen_match(G *g, Expr *e);
static const char *gen_if(G *g, Expr *e);
static const char *gen_fn_expr(G *g, Expr *e);

static const char *gen_expr(G *g, Expr *e) {
  Ty *t = sub(g, e->ty);
  switch (e->kind) {
    case E_INT: return sfmt(g, "INT64_C(%s)", cs(g, e->text));
    case E_FLOAT: return cs(g, e->text);
    case E_BOOL: return e->bool_val ? "true" : "false";
    case E_STRING: {
      if (e->args.len == 0) return str_lit(g, e->segs.items[0]);
      const char *sb = newtmp(g);
      emit(g, "yu_sb %s = {0};", sb);
      for (int i = 0; i < e->segs.len; i++) {
        if (e->segs.items[i].len) emit(g, "yu_sb_str(&%s, %s);", sb, str_lit(g, e->segs.items[i]));
        if (i < e->args.len) {
          Expr *x = e->args.items[i];
          const char *v = gen_expr(g, x);
          if (sub(g, x->ty)->kind == TY_STRING) emit(g, "yu_sb_str(&%s, %s);", sb, v);
          else emit(g, "%s(&%s, %s);", fmt_fn(g, x->ty), sb, v);
        }
      }
      return sfmt(g, "yu_sb_finish(&%s)", sb);
    }
    case E_IDENT: {
      if (str_eq_c(e->text, "Unit")) return "YU_UNIT";
      if (str_eq_c(e->text, "None")) return sfmt(g, "(%s){false}", ctype(g, t));
      if (e->call_kind == CK_FN) {
        Decl *d = find_decl(&g->fns, e->text);
        return sfmt(g, "(%s){%s, NULL}", ctype(g, t), fn_thunk(g, d));
      }
      if (e->call_kind == CK_STATIC) return fn_cname(g, e->text);
      return bind_ref(g, e->text);
    }
    case E_FIELD: {
      Expr *obj = e->a;
      if (obj->kind == E_IDENT && obj->call_kind == CK_UNKNOWN && t->kind == TY_ENUM) {
        // unit variant: Status.Active
        return sfmt(g, "(%s){ .tag = yu_%s_%s }", ctype(g, t), mangle(g, t), cs(g, e->text));
      }
      const char *o = gen_place(g, obj);
      if (sub(g, obj->ty)->kind == TY_ENTRY) return sfmt(g, "%s.%s", o, cs(g, e->text));
      return sfmt(g, "%s.f_%s", o, cs(g, e->text));
    }
    case E_INDEX: {
      const char *l = gen_place(g, e->a);
      const char *i = gen_expr(g, e->b);
      return sfmt(g, "(*(%s *)yu_list_at(%s, %s, sizeof(%s), %s))", ctype(g, t), l, i, ctype(g, t), loc(g, e->line));
    }
    case E_CALL: return gen_call(g, e);
    case E_UNARY: {
      const char *v = gen_expr(g, e->a);
      if (e->op == T_BANG) return sfmt(g, "(!%s)", v);
      if (t->kind == TY_FLOAT) return sfmt(g, "(-%s)", v);
      return sfmt(g, "yu_neg(%s, %s)", v, loc(g, e->line));
    }
    case E_BINARY: {
      TokKind op = e->op;
      if (op == T_ANDAND || op == T_OROR) {
        const char *r = newtmp(g);
        emit(g, "bool %s = %s;", r, gen_expr(g, e->a));
        emit(g, "if (%s%s) {", op == T_ANDAND ? "" : "!", r);
        g->indent++;
        emit(g, "%s = %s;", r, gen_expr(g, e->b));
        g->indent--;
        emit(g, "}");
        return r;
      }
      if (op == T_DOTDOT) {
        const char *r = newtmp(g);
        const char *lo = gen_temp(g, e->a), *hi = gen_temp(g, e->b);
        emit(g, "yu_list %s = yu_list_new(sizeof(int64_t));", r);
        emit(g, "for (int64_t i_ = %s; i_ < %s; i_++) yu_list_push(&%s, &i_, sizeof(int64_t));", lo, hi, r);
        return r;
      }
      const char *l = gen_expr(g, e->a);
      const char *r = gen_expr(g, e->b);
      Ty *lt = sub(g, e->a->ty);
      bool is_int = lt->kind == TY_INT, is_float = lt->kind == TY_FLOAT, is_str = lt->kind == TY_STRING;
      switch (op) {
        case T_PLUS: return is_int ? sfmt(g, "yu_add(%s, %s, %s)", l, r, loc(g, e->line)) : sfmt(g, "(%s + %s)", l, r);
        case T_MINUS: return is_int ? sfmt(g, "yu_sub(%s, %s, %s)", l, r, loc(g, e->line)) : sfmt(g, "(%s - %s)", l, r);
        case T_STAR: return is_int ? sfmt(g, "yu_mul(%s, %s, %s)", l, r, loc(g, e->line)) : sfmt(g, "(%s * %s)", l, r);
        case T_SLASH: return is_int ? sfmt(g, "yu_div(%s, %s, %s)", l, r, loc(g, e->line)) : sfmt(g, "(%s / %s)", l, r);
        case T_PERCENT: return is_int ? sfmt(g, "yu_mod(%s, %s, %s)", l, r, loc(g, e->line)) : sfmt(g, "fmod(%s, %s)", l, r);
        case T_EQ: case T_NE: {
          const char *cmp;
          if (is_int || is_float || lt->kind == TY_BOOL) cmp = sfmt(g, "(%s == %s)", l, r);
          else cmp = sfmt(g, "%s(%s, %s)", eq_fn(g, lt), l, r);
          return op == T_EQ ? cmp : sfmt(g, "(!%s)", cmp);
        }
        default: {
          const char *o = tok_name(op);
          if (is_str) return sfmt(g, "(yu_str_cmp(%s, %s) %s 0)", l, r, o);
          return sfmt(g, "(%s %s %s)", l, o, r);
        }
      }
    }
    case E_TRY: {
      Expr *x = e->a;
      const char *v = gen_temp(g, x);
      Ty *ret = g->fn_ret;
      Ty *xt = sub(g, x->ty);
      if (xt->kind == TY_RESULT) {
        if (xt->b->kind == TY_ENUM) emit(g, "if (!%s.ok) yu_trace_add(&%s.err.trace_, %s);", v, v, loc(g, e->line));
        emit(g, "if (!%s.ok) return (%s){ .ok = false, .err = %s.err };", v, ctype(g, ret), v);
      } else {
        emit(g, "if (!%s.some) return (%s){false};", v, ctype(g, ret));
      }
      return sfmt(g, "%s.val", v);
    }
    case E_IF: return gen_if(g, e);
    case E_MATCH: return gen_match(g, e);
    case E_STRUCT: {
      const char *r = newtmp(g);
      if (e->base) emit(g, "%s %s = %s;", ctype(g, t), r, gen_value(g, e->base));
      else emit(g, "%s %s = {0};", ctype(g, t), r);
      for (int i = 0; i < e->fields.len; i++)
        emit(g, "%s.f_%s = %s;", r, cs(g, e->fields.items[i].name), gen_value(g, e->fields.items[i].value));
      return r;
    }
    case E_LIST: {
      const char *r = newtmp(g);
      const char *et = ctype(g, t->a);
      emit(g, "yu_list %s = yu_list_new(sizeof(%s));", r, et);
      for (int i = 0; i < e->args.len; i++) {
        const char *v = newtmp(g);
        emit(g, "%s %s = %s;", et, v, gen_value(g, e->args.items[i]));
        emit(g, "yu_list_push(&%s, &%s, sizeof(%s));", r, v, et);
      }
      return r;
    }
    case E_FN: return gen_fn_expr(g, e);
    case E_BLOCK: {
      if (t->kind == TY_UNIT || t->kind == TY_NEVER) { emit(g, "{"); g->indent++; gen_block_into(g, e->block, NULL, t); g->indent--; emit(g, "}"); return "YU_UNIT"; }
      const char *r = newtmp(g);
      emit(g, "%s %s;", ctype(g, t), r);
      emit(g, "{");
      g->indent++;
      gen_block_into(g, e->block, r, t);
      g->indent--;
      emit(g, "}");
      return r;
    }
    case E_TYPECTOR:
      if (t->kind == TY_LIST) return sfmt(g, "yu_list_new(sizeof(%s))", ctype(g, t->a));
      return sfmt(g, "yu_map_new(%d, sizeof(%s))", map_kkind(t->a), ctype(g, t->b));
    case E_PAREN: return "YU_UNIT";
  }
  return "YU_UNIT";
}

// A place expression (no copy): used for receivers, field access and assignment targets.
static const char *gen_place(G *g, Expr *e) {
  switch (e->kind) {
    case E_IDENT: return bind_ref(g, e->text);
    case E_FIELD: {
      const char *o = gen_place(g, e->a);
      if (e->a->ty->kind == TY_ENTRY) return sfmt(g, "%s.%s", o, cs(g, e->text));
      return sfmt(g, "%s.f_%s", o, cs(g, e->text));
    }
    case E_INDEX: {
      const char *l = gen_place(g, e->a);
      const char *i = gen_expr(g, e->b);
      return sfmt(g, "(*(%s *)yu_list_at(%s, %s, sizeof(%s), %s))", ctype(g, e->ty), l, i, ctype(g, e->ty), loc(g, e->line));
    }
    default: return gen_temp(g, e);
  }
}

// Assigns the value of `e` to `target` (or evaluates it when target is NULL / type is Never).
static void gen_assign_value(G *g, Expr *e, const char *target, Ty *ty) {
  const char *v = gen_expr(g, e);
  if (!target || !ty || ty->kind == TY_UNIT || ty->kind == TY_NEVER || (e->ty && sub(g, e->ty)->kind == TY_NEVER)) {
    if (e->kind != E_IF && e->kind != E_MATCH && e->kind != E_BLOCK) emit(g, "(void)(%s);", v);
    return;
  }
  emit(g, "%s = %s;", target, v);
}

// Emits the statements of a block; the last expression goes to `target` when given.
static void gen_block_into(G *g, Block *b, const char *target, Ty *ty) {
  int mark = scope_push(g);
  for (int i = 0; i < b->stmts.len; i++) {
    Stmt *s = b->stmts.items[i];
    bool last = i == b->stmts.len - 1;
    if (last && s->kind == S_EXPR) gen_assign_value(g, s->value, target, ty);
    else {
      Block one = {0};
      VEC_PUSH(g->a, &one.stmts, s);
      gen_stmts(g, &one);
    }
  }
  scope_pop(g, mark);
}

static const char *gen_if(G *g, Expr *e) {
  Ty *t = sub(g, e->ty);
  bool has_value = t->kind != TY_UNIT && t->kind != TY_NEVER && (e->else_block || e->else_if);
  const char *r = NULL;
  if (has_value) { r = newtmp(g); emit(g, "%s %s;", ctype(g, t), r); }
  Expr *cur = e;
  for (;;) {
    const char *c = gen_expr(g, cur->a);
    emit(g, "if (%s) {", c);
    g->indent++;
    gen_block_into(g, cur->block, r, t);
    g->indent--;
    if (cur->else_if) {
      emit(g, "} else {");
      g->indent++;
      // generate the chained if inside the else block so its condition's statements stay ordered
      Expr *nx = cur->else_if;
      const char *c2 = gen_expr(g, nx->a);
      emit(g, "if (%s) {", c2);
      g->indent++;
      gen_block_into(g, nx->block, r, t);
      g->indent--;
      if (nx->else_if || nx->else_block) {
        // continue the chain by rewriting: treat nx as current and close later
        Expr tmp = *nx;
        tmp.a = NULL;
        // emit the rest recursively
        if (nx->else_if) {
          emit(g, "} else {");
          g->indent++;
          Expr rest = *nx->else_if;
          Expr wrapper = {0};
          wrapper.kind = E_IF;
          wrapper.ty = t;
          wrapper.a = rest.a;
          wrapper.block = rest.block;
          wrapper.else_if = rest.else_if;
          wrapper.else_block = rest.else_block;
          const char *rr = r;
          // reuse: generate nested chain writing into the same temp
          {
            const char *c3 = gen_expr(g, wrapper.a);
            emit(g, "if (%s) {", c3);
            g->indent++;
            gen_block_into(g, wrapper.block, rr, t);
            g->indent--;
            Expr *deeper = &wrapper;
            while (deeper->else_if) {
              deeper = deeper->else_if;
              emit(g, "} else {");
              g->indent++;
              const char *c4 = gen_expr(g, deeper->a);
              emit(g, "if (%s) {", c4);
              g->indent++;
              gen_block_into(g, deeper->block, rr, t);
              g->indent--;
            }
            if (deeper->else_block) {
              emit(g, "} else {");
              g->indent++;
              gen_block_into(g, deeper->else_block, rr, t);
              g->indent--;
            }
            emit(g, "}");
            // close the nested else blocks opened in the while loop
            Expr *walk = &wrapper;
            while (walk->else_if) { walk = walk->else_if; g->indent--; emit(g, "}"); }
          }
          g->indent--;
          emit(g, "}");
        } else {
          emit(g, "} else {");
          g->indent++;
          gen_block_into(g, nx->else_block, r, t);
          g->indent--;
          emit(g, "}");
        }
        (void)tmp;
      } else {
        emit(g, "}");
      }
      g->indent--;
      emit(g, "}");
      break;
    }
    if (cur->else_block) {
      emit(g, "} else {");
      g->indent++;
      gen_block_into(g, cur->else_block, r, t);
      g->indent--;
    }
    emit(g, "}");
    break;
  }
  return r ? r : "YU_UNIT";
}

// Pattern test: returns a C condition; bindings are emitted by gen_pattern_bind.
static const char *pat_cond(G *g, Pattern *p, const char *subj, Ty *t) {
  switch (p->kind) {
    case PAT_WILD: case PAT_BIND: return "true";
    case PAT_LIT: {
      const char *v = gen_expr(g, p->lit);
      if (t->kind == TY_STRING) return sfmt(g, "yu_str_eq(%s, %s)", subj, v);
      return sfmt(g, "(%s == %s)", subj, v);
    }
    case PAT_VARIANT: {
      Str head = p->path.items[p->path.len - 1];
      if (p->path.len == 1) {
        if (str_eq_c(head, "None")) return sfmt(g, "(!%s.some)", subj);
        if (str_eq_c(head, "Some")) { const char *sub = pat_cond(g, p->fields.items[0].pat, sfmt(g, "%s.val", subj), t->a); return sfmt(g, "(%s.some && %s)", subj, sub); }
        if (str_eq_c(head, "Ok")) { const char *sub = pat_cond(g, p->fields.items[0].pat, sfmt(g, "%s.val", subj), t->a); return sfmt(g, "(%s.ok && %s)", subj, sub); }
        if (str_eq_c(head, "Err")) { const char *sub = pat_cond(g, p->fields.items[0].pat, sfmt(g, "%s.err", subj), t->b); return sfmt(g, "(!%s.ok && %s)", subj, sub); }
        return "false";
      }
      Decl *d = t->decl;
      Variant *v = NULL;
      int vi = 0;
      for (int i = 0; i < d->variants.len; i++) if (str_eq(d->variants.items[i].name, head)) { v = &d->variants.items[i]; vi = i; }
      if (!v) return "false";
      Buf b = {0};
      buf_printf(&b, "(%s.tag == %d", subj, vi);
      for (int i = 0; i < p->fields.len; i++) {
        PatField *f = &p->fields.items[i];
        Str fname = f->name.len ? f->name : v->fields.items[i].name;
        Ty *ft = NULL;
        for (int k = 0; k < v->fields.len; k++) if (str_eq(v->fields.items[k].name, fname)) ft = ty_subst(g->a, v->fields.items[k].ty, &d->generics, &t->args);
        const char *sub = pat_cond(g, f->pat, sfmt(g, "%s.u.v_%s.f_%s", subj, cs(g, v->name), cs(g, fname)), ft);
        if (strcmp(sub, "true") != 0) buf_printf(&b, " && %s", sub);
      }
      buf_puts(&b, ")");
      const char *r = arena_cstr(g->a, str_from(b.p, b.len));
      buf_free(&b);
      return r;
    }
  }
  return "true";
}

static void pat_bind(G *g, Pattern *p, const char *subj, Ty *t) {
  switch (p->kind) {
    case PAT_WILD: case PAT_LIT: return;
    case PAT_BIND: { const char *cn = bind_new(g, p->name); emit(g, "%s %s = %s;", ctype(g, t), cn, subj); return; }
    case PAT_VARIANT: {
      Str head = p->path.items[p->path.len - 1];
      if (p->path.len == 1) {
        if (str_eq_c(head, "Some")) pat_bind(g, p->fields.items[0].pat, sfmt(g, "%s.val", subj), t->a);
        else if (str_eq_c(head, "Ok")) pat_bind(g, p->fields.items[0].pat, sfmt(g, "%s.val", subj), t->a);
        else if (str_eq_c(head, "Err")) pat_bind(g, p->fields.items[0].pat, sfmt(g, "%s.err", subj), t->b);
        return;
      }
      Decl *d = t->decl;
      Variant *v = NULL;
      for (int i = 0; i < d->variants.len; i++) if (str_eq(d->variants.items[i].name, head)) v = &d->variants.items[i];
      if (!v) return;
      for (int i = 0; i < p->fields.len; i++) {
        PatField *f = &p->fields.items[i];
        Str fname = f->name.len ? f->name : v->fields.items[i].name;
        Ty *ft = NULL;
        for (int k = 0; k < v->fields.len; k++) if (str_eq(v->fields.items[k].name, fname)) ft = ty_subst(g->a, v->fields.items[k].ty, &d->generics, &t->args);
        pat_bind(g, f->pat, sfmt(g, "%s.u.v_%s.f_%s", subj, cs(g, v->name), cs(g, fname)), ft);
      }
      return;
    }
  }
}

static const char *gen_match(G *g, Expr *e) {
  Ty *t = sub(g, e->ty);
  Ty *st = sub(g, e->a->ty);
  const char *subj = gen_temp(g, e->a);
  bool has_value = t->kind != TY_UNIT && t->kind != TY_NEVER;
  const char *r = NULL;
  if (has_value) { r = newtmp(g); emit(g, "%s %s;", ctype(g, t), r); }
  const char *done = newtmp(g);
  emit(g, "bool %s = false;", done);
  for (int i = 0; i < e->arms.len; i++) {
    MatchArm *arm = &e->arms.items[i];
    emit(g, "if (!%s) {", done);
    g->indent++;
    const char *cond = pat_cond(g, arm->pat, subj, st);
    emit(g, "if (%s) {", cond);
    g->indent++;
    int arm_mark = scope_push(g);
    pat_bind(g, arm->pat, subj, st);
    if (arm->guard) {
      const char *gc = gen_expr(g, arm->guard);
      emit(g, "if (%s) {", gc);
      g->indent++;
    }
    emit(g, "%s = true;", done);
    if (arm->body->kind == E_BLOCK) gen_block_into(g, arm->body->block, r, t);
    else gen_assign_value(g, arm->body, r, t);
    if (arm->guard) { g->indent--; emit(g, "}"); }
    scope_pop(g, arm_mark);
    g->indent--;
    emit(g, "}");
    g->indent--;
    emit(g, "}");
  }
  return r ? r : "YU_UNIT";
}

// Anonymous function: a static C function taking its environment first.
static const char *mangle_binds(G *g, TyVec *binds);

static const char *gen_fn_expr(G *g, Expr *e) {
  Ty *ft = sub(g, e->ty);
  // inside a generic instance the same anonymous function is generated once per instance
  const char *inst = g->sub_names ? mangle_binds(g, g->sub_binds) : "";
  const char *name = sfmt(g, "yu_anon_%d%s", e->id, inst);
  const char *env_t = sfmt(g, "struct yu_env_%d%s", e->id, inst);
  // environment struct
  Buf env = {0};
  buf_printf(&env, "%s {", env_t);
  for (int i = 0; i < e->cap_names.len; i++) buf_printf(&env, " %s %s;", ctype(g, (Ty *)e->cap_types.items[i]), var_cname(g, e->cap_names.items[i]));
  if (e->cap_names.len == 0) buf_puts(&env, " char unused_;");
  buf_puts(&env, " };\n");
  buf_puts(&g->types, env.p);
  buf_free(&env);
  // prototype
  // closure value: capture the outer bindings before the inner scope shadows them
  Buf capinit = {0};
  for (int i = 0; i < e->cap_names.len; i++)
    buf_printf(&capinit, "->%s = %s;\n", var_cname(g, e->cap_names.items[i]), bind_ref(g, e->cap_names.items[i]));
  int fn_mark = scope_push(g);
  Buf sig = {0};
  buf_printf(&sig, "static %s %s(void *env_", ctype(g, ft->ret), name);
  const char **pnames = arena_alloc(g->a, sizeof(char *) * (size_t)(e->params.len + 1));
  for (int i = 0; i < e->params.len; i++) {
    pnames[i] = bind_new(g, e->params.items[i].name);
    buf_printf(&sig, ", %s %s", ctype(g, e->params.items[i].ty), pnames[i]);
  }
  buf_puts(&sig, ")");
  buf_printf(&g->protos, "%s;\n", sig.p);
  // body, generated into its own buffer so the enclosing function is not interrupted
  Buf *saved_out = g->out;
  Ty *saved_ret = g->fn_ret;
  int saved_indent = g->indent;
  Buf body = {0};
  g->out = &body;
  g->fn_ret = ft->ret;
  g->indent = 1;
  emit(g, "(void)env_;");
  for (int i = 0; i < e->cap_names.len; i++) {
    const char *cn = bind_new(g, e->cap_names.items[i]);
    emit(g, "%s %s = ((%s *)env_)->%s; (void)%s;", ctype(g, (Ty *)e->cap_types.items[i]), cn, env_t, var_cname(g, e->cap_names.items[i]), cn);
  }
  for (int i = 0; i < e->params.len; i++) emit(g, "(void)%s;", pnames[i]);
  if (ft->ret->kind == TY_UNIT) { gen_block_into(g, e->block, NULL, NULL); emit(g, "return YU_UNIT;"); }
  else {
    const char *r = newtmp(g);
    emit(g, "%s %s = {0};", ctype(g, ft->ret), r);
    gen_block_into(g, e->block, r, ft->ret);
    emit(g, "return %s;", r);
  }
  g->out = saved_out;
  g->fn_ret = saved_ret;
  g->indent = saved_indent;
  scope_pop(g, fn_mark);
  buf_printf(&g->code, "%s {\n%s}\n\n", sig.p, body.p);
  buf_free(&sig);
  buf_free(&body);
  const char *ev = newtmp(g);
  emit(g, "%s *%s = yu_alloc(sizeof(%s));", env_t, ev, env_t);
  // capinit lines are "->field = value;\n": prefix each with the env pointer
  for (char *line = capinit.p; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    emit(g, "%s%s", ev, line);
    if (!nl) break;
    line = nl + 1;
  }
  buf_free(&capinit);
  return sfmt(g, "(%s){%s, %s}", ctype(g, ft), name, ev);
}

// ---- calls -------------------------------------------------------------

static const char *gen_args(G *g, Expr *call, const char *first) {
  Buf b = {0};
  if (first) buf_puts(&b, first);
  for (int i = 0; i < call->args.len; i++) {
    const char *v = gen_value(g, call->args.items[i]);
    if (b.len) buf_puts(&b, ", ");
    buf_puts(&b, v);
  }
  const char *r = arena_cstr(g->a, str_from(b.p ? b.p : "", b.len));
  buf_free(&b);
  return r;
}

static const char *gen_builtin(G *g, Expr *e) {
  Expr *callee = e->a;
  Expr *obj = callee->a;
  Str name = callee->text;
  Ty *rt = sub(g, obj->ty);
  Ty *t = sub(g, e->ty);
  const char *L = loc(g, e->line);
#define IS(n) str_eq_c(name, n)
  switch (rt->kind) {
    case TY_LIST: {
      const char *et = ctype(g, rt->a);
      if (IS("push")) {
        const char *p = gen_place(g, obj);
        const char *v = newtmp(g);
        emit(g, "%s %s = %s;", et, v, gen_value(g, e->args.items[0]));
        emit(g, "yu_list_push(&%s, &%s, sizeof(%s));", p, v, et);
        return "YU_UNIT";
      }
      const char *l = gen_place(g, obj);
      if (IS("len")) return sfmt(g, "%s.len", l);
      if (IS("get")) {
        const char *r = newtmp(g), *i = gen_temp(g, e->args.items[0]);
        emit(g, "%s %s = {false};", ctype(g, t), r);
        emit(g, "if (%s >= 0 && %s < %s.len) { %s.some = true; %s.val = ((%s *)%s.p)[%s]; }", i, i, l, r, r, et, l, i);
        return r;
      }
      if (IS("contains")) {
        const char *r = newtmp(g), *x = gen_temp(g, e->args.items[0]);
        emit(g, "bool %s = false;", r);
        emit(g, "for (int64_t i_ = 0; i_ < %s.len && !%s; i_++) if (%s(((%s *)%s.p)[i_], %s)) %s = true;", l, r, eq_fn(g, rt->a), et, l, x, r);
        return r;
      }
      if (IS("map") || IS("filter")) {
        const char *f = gen_temp(g, e->args.items[0]);
        const char *r = newtmp(g);
        bool is_map = IS("map");
        const char *ot = is_map ? ctype(g, t->a) : et;
        emit(g, "yu_list %s = yu_list_new(sizeof(%s));", r, ot);
        emit(g, "for (int64_t i_ = 0; i_ < %s.len; i_++) {", l);
        g->indent++;
        if (is_map) {
          emit(g, "%s v_ = %s.fn(%s.env, ((%s *)%s.p)[i_]);", ot, f, f, et, l);
          emit(g, "yu_list_push(&%s, &v_, sizeof(%s));", r, ot);
        } else {
          emit(g, "if (%s.fn(%s.env, ((%s *)%s.p)[i_])) yu_list_push(&%s, &((%s *)%s.p)[i_], sizeof(%s));", f, f, et, l, r, et, l, et);
        }
        g->indent--;
        emit(g, "}");
        return r;
      }
      break;
    }
    case TY_MAP: {
      const char *kt = ctype(g, rt->a), *vt = ctype(g, rt->b);
      if (IS("set")) {
        const char *p = gen_place(g, obj);
        const char *k = newtmp(g), *v = newtmp(g);
        emit(g, "%s %s = %s;", kt, k, gen_value(g, e->args.items[0]));
        emit(g, "%s %s = %s;", vt, v, gen_value(g, e->args.items[1]));
        emit(g, "yu_map_set(&%s, &%s, &%s, sizeof(%s));", p, k, v, vt);
        return "YU_UNIT";
      }
      if (IS("remove")) {
        const char *p = gen_place(g, obj);
        const char *k = newtmp(g);
        emit(g, "%s %s = %s;", kt, k, gen_value(g, e->args.items[0]));
        emit(g, "yu_map_remove(&%s, &%s, sizeof(%s));", p, k, vt);
        return "YU_UNIT";
      }
      const char *m = gen_place(g, obj);
      if (IS("len")) return sfmt(g, "yu_map_len(%s)", m);
      if (IS("get")) {
        const char *r = newtmp(g), *k = newtmp(g);
        emit(g, "%s %s = %s;", kt, k, gen_value(g, e->args.items[0]));
        emit(g, "%s %s = {false};", ctype(g, t), r);
        emit(g, "%s.some = yu_map_get(%s, &%s, &%s.val, sizeof(%s));", r, m, k, r, vt);
        return r;
      }
      if (IS("has")) {
        const char *k = newtmp(g);
        emit(g, "%s %s = %s;", kt, k, gen_value(g, e->args.items[0]));
        return sfmt(g, "yu_map_has(%s, &%s)", m, k);
      }
      if (IS("entries")) {
        const char *ent = ctype(g, t->a);
        return sfmt(g, "yu_map_entries(%s, sizeof(%s), offsetof(%s, value), sizeof(%s))", m, vt, ent, ent);
      }
      break;
    }
    case TY_OPTION: {
      const char *o = gen_temp(g, obj);
      if (IS("is_some")) return sfmt(g, "%s.some", o);
      if (IS("unwrap_or")) {
        const char *d = gen_temp(g, e->args.items[0]);
        return sfmt(g, "(%s.some ? %s.val : %s)", o, o, d);
      }
      if (IS("ok_or")) {
        const char *r = newtmp(g);
        emit(g, "%s %s = {0};", ctype(g, t), r);
        emit(g, "if (%s.some) { %s.ok = true; %s.val = %s.val; } else { %s.ok = false; %s.err = %s; }", o, r, r, o, r, r, gen_value(g, e->args.items[0]));
        return r;
      }
      break;
    }
    case TY_RESULT: {
      const char *o = gen_temp(g, obj);
      if (IS("is_ok")) return sfmt(g, "%s.ok", o);
      if (IS("unwrap_or")) {
        const char *d = gen_temp(g, e->args.items[0]);
        return sfmt(g, "(%s.ok ? %s.val : %s)", o, o, d);
      }
      break;
    }
    case TY_ENUM:
      if (IS("trace")) { const char *o = gen_temp(g, obj); return sfmt(g, "yu_list_copy(%s.trace_, sizeof(yu_str))", o); }
      break;
    case TY_CAP: {
      const char *cap = gen_temp(g, obj);
      if (IS("now")) return sfmt(g, "yu_clock_now(%s)", cap);
      if (IS("get")) {
        const char *r = newtmp(g), *nm = gen_temp(g, e->args.items[0]);
        emit(g, "%s %s = {false};", ctype(g, t), r);
        emit(g, "%s.some = yu_env_get(%s, %s, &%s.val);", r, cap, nm, r);
        return r;
      }
      if (IS("lines")) return sfmt(g, "yu_log_lines(%s)", cap);
      if (IS("info") || IS("error")) {
        const char *msg = gen_expr(g, e->args.items[0]);
        emit(g, "yu_log_write(%s, \"%s\", %s);", cap, cs(g, name), msg);
        return "YU_UNIT";
      }
      break;
    }
    case TY_STORE: {
      Ty *T = rt->a;
      const char *st = gen_temp(g, obj);
      const char *tt = ctype(g, T);
      if (IS("get")) {
        // a copy into the caller's arena: nothing from the store's arena escapes
        const char *r = newtmp(g);
        if (needs_copy(g, T)) emit(g, "%s %s = %s(*(%s *)%s->value);", tt, r, copy_fn(g, T), tt, st);
        else emit(g, "%s %s = *(%s *)%s->value;", tt, r, tt, st);
        return r;
      }
      if (IS("set") || IS("update") || IS("try_update")) {
        const char *nv = newtmp(g);
        const char *res = NULL;
        if (IS("set")) emit(g, "%s %s = %s;", tt, nv, gen_value(g, e->args.items[0]));
        else {
          const char *f = gen_temp(g, e->args.items[0]);
          const char *cur = newtmp(g);
          if (needs_copy(g, T)) emit(g, "%s %s = %s(*(%s *)%s->value);", tt, cur, copy_fn(g, T), tt, st);
          else emit(g, "%s %s = *(%s *)%s->value;", tt, cur, tt, st);
          if (IS("update")) {
            emit(g, "%s %s;", tt, nv);
            emit(g, "if (yu_catch_begin()) { %s = %s.fn(%s.env, %s); yu_catch_end(); } else { yu_fail(\"R-STORE-01\", %s, \"update function panicked; value unchanged\"); }", nv, f, f, cur, loc(g, e->line));
          } else {
            res = newtmp(g);
            emit(g, "%s %s;", ctype(g, t), res);
            emit(g, "if (yu_catch_begin()) { %s = %s.fn(%s.env, %s); yu_catch_end(); } else { yu_fail(\"R-STORE-01\", %s, \"update function panicked; value unchanged\"); }", res, f, f, cur, loc(g, e->line));
            emit(g, "%s %s = %s.val;", tt, nv, res);
            emit(g, "if (%s.ok) {", res);
            g->indent++;
          }
        }
        const char *prev = newtmp(g), *cp = newtmp(g);
        emit(g, "yu_arena *%s = yu_store_begin(%s);", prev, st);
        if (needs_copy(g, T)) emit(g, "%s %s = %s(%s);", tt, cp, copy_fn(g, T), nv);
        else emit(g, "%s %s = %s;", tt, cp, nv);
        emit(g, "yu_store_commit(%s, &%s, %s);", st, cp, prev);
        if (IS("try_update")) { g->indent--; emit(g, "}"); return res; }
        if (IS("update")) return nv;
        return "YU_UNIT";
      }
      break;
    }
    case TY_STRING: {
      const char *s = gen_expr(g, obj);
      if (IS("len")) return sfmt(g, "yu_str_len(%s)", s);
      if (IS("is_empty")) return sfmt(g, "yu_str_is_empty(%s)", s);
      if (IS("split")) return sfmt(g, "yu_str_split(%s, %s)", s, gen_expr(g, e->args.items[0]));
      if (IS("trim")) return sfmt(g, "yu_str_trim(%s)", s);
      if (IS("starts_with")) return sfmt(g, "yu_str_starts_with(%s, %s)", s, gen_expr(g, e->args.items[0]));
      if (IS("ends_with")) return sfmt(g, "yu_str_ends_with(%s, %s)", s, gen_expr(g, e->args.items[0]));
      if (IS("contains")) return sfmt(g, "yu_str_contains(%s, %s)", s, gen_expr(g, e->args.items[0]));
      if (IS("find")) {
        const char *r = newtmp(g);
        emit(g, "%s %s = {false};", ctype(g, t), r);
        emit(g, "%s.some = yu_str_find(%s, %s, &%s.val);", r, s, gen_expr(g, e->args.items[0]), r);
        return r;
      }
      if (IS("replace")) { const char *a0 = gen_expr(g, e->args.items[0]); const char *a1 = gen_expr(g, e->args.items[1]); return sfmt(g, "yu_str_replace(%s, %s, %s)", s, a0, a1); }
      if (IS("to_upper")) return sfmt(g, "yu_str_to_upper(%s)", s);
      if (IS("to_lower")) return sfmt(g, "yu_str_to_lower(%s)", s);
      if (IS("slice")) { const char *a0 = gen_expr(g, e->args.items[0]); const char *a1 = gen_expr(g, e->args.items[1]); return sfmt(g, "yu_str_slice(%s, %s, %s, %s)", s, a0, a1, L); }
      if (IS("chars")) return sfmt(g, "yu_str_chars(%s)", s);
      if (IS("bytes")) return sfmt(g, "yu_str_bytes(%s)", s);
      if (IS("join")) return sfmt(g, "yu_str_join(%s, %s)", s, gen_expr(g, e->args.items[0]));
      if (IS("repeat")) return sfmt(g, "yu_str_repeat(%s, %s, %s)", s, gen_expr(g, e->args.items[0]), L);
      break;
    }
    default: break;
  }
#undef IS
  emit(g, "/* unsupported builtin %s */", cs(g, name));
  return "YU_UNIT";
}

static void gen_fn_instance(G *g, Decl *d, Ty *self_type, StrVec *names, TyVec *binds, const char *cname);

// Joins the mangled names of `binds` for an instance name.
static const char *mangle_binds(G *g, TyVec *binds) {
  Buf b = {0};
  for (int i = 0; i < binds->len; i++) buf_printf(&b, "_%s", mangle(g, binds->items[i]));
  const char *r = arena_cstr(g->a, str_from(b.p ? b.p : "", b.len));
  buf_free(&b);
  return r;
}

// JSON encoder per type: appends JSON text for a value to a builder.
static const char *jenc_fn(G *g, Ty *t) {
  t = sub(g, t);
  const char *n = sfmt(g, "yu_jenc_%s", mangle(g, t));
  if (seen(&g->fmt_done, n)) return n;
  mark(g, &g->fmt_done, n);
  const char *ct = ctype(g, t);
  buf_printf(&g->protos, "static void %s(yu_sb *b, %s v);\n", n, ct);
  Buf b = {0};
  buf_printf(&b, "static void %s(yu_sb *b, %s v) {\n", n, ct);
  switch (t->kind) {
    case TY_INT: buf_puts(&b, "  yu_sb_int(b, v);\n"); break;
    case TY_FLOAT: buf_puts(&b, "  yu_sb_float(b, v);\n"); break;
    case TY_BOOL: buf_puts(&b, "  yu_sb_bool(b, v);\n"); break;
    case TY_STRING: buf_puts(&b, "  yu_json_quote(b, v);\n"); break;
    case TY_OPTION: buf_printf(&b, "  if (v.some) %s(b, v.val); else yu_sb_cstr(b, \"null\");\n", jenc_fn(g, t->a)); break;
    case TY_LIST:
      buf_printf(&b, "  yu_sb_cstr(b, \"[\");\n  for (int64_t i = 0; i < v.len; i++) { if (i) yu_sb_cstr(b, \",\"); %s(b, ((%s *)v.p)[i]); }\n  yu_sb_cstr(b, \"]\");\n", jenc_fn(g, t->a), ctype(g, t->a));
      break;
    case TY_MAP:
      buf_printf(&b, "  yu_sb_cstr(b, \"{\");\n  for (int64_t i = 0; i < v.keys.len; i++) { if (i) yu_sb_cstr(b, \",\"); yu_json_quote(b, ((yu_str *)v.keys.p)[i]); yu_sb_cstr(b, \":\"); %s(b, ((%s *)v.vals.p)[i]); }\n  yu_sb_cstr(b, \"}\");\n", jenc_fn(g, t->b), ctype(g, t->b));
      break;
    case TY_STRUCT: {
      buf_puts(&b, "  yu_sb_cstr(b, \"{\");\n");
      for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        const char *fn = cs(g, t->decl->params.items[i].name);
        buf_printf(&b, "  yu_sb_cstr(b, \"%s\\\"%s\\\":\"); %s(b, v.f_%s);\n", i ? "," : "", fn, jenc_fn(g, ft), fn);
      }
      buf_puts(&b, "  yu_sb_cstr(b, \"}\");\n");
      break;
    }
    case TY_ENUM:
      for (int i = 0; i < t->decl->variants.len; i++)
        buf_printf(&b, "  if (v.tag == %d) { yu_sb_cstr(b, \"\\\"%s\\\"\"); return; }\n", i, cs(g, t->decl->variants.items[i].name));
      break;
    default: buf_puts(&b, "  (void)v; yu_sb_cstr(b, \"null\");\n"); break;
  }
  buf_puts(&b, "}\n");
  buf_puts(&g->helpers, b.p);
  buf_free(&b);
  return n;
}

// JSON decoder per type: fills *out from a parsed node; on a shape error fills err and returns false.
static const char *jdec_fn(G *g, Ty *t) {
  t = sub(g, t);
  const char *n = sfmt(g, "yu_jdec_%s", mangle(g, t));
  if (seen(&g->copy_done, n)) return n;
  mark(g, &g->copy_done, n);
  const char *ct = ctype(g, t);
  buf_printf(&g->protos, "static bool %s(yu_jv *v, %s *out, yu_sb *path, yu_json_err *err);\n", n, ct);
  Buf b = {0};
  buf_printf(&b, "static bool %s(yu_jv *v, %s *out, yu_sb *path, yu_json_err *err) {\n", n, ct);
  const char *tname = ty_str(g->a, t);
  const char *fail = sfmt(g, "{ err->path = yu_sb_finish(path); err->expected = \"%s\"; return false; }", tname);
  switch (t->kind) {
    case TY_INT: buf_printf(&b, "  if (!yu_jv_int(v, out)) %s\n  return true;\n", fail); break;
    case TY_FLOAT: buf_printf(&b, "  if (!yu_jv_float(v, out)) %s\n  return true;\n", fail); break;
    case TY_BOOL: buf_printf(&b, "  if (!v || v->kind != YU_JV_BOOL) %s\n  *out = v->b;\n  return true;\n", fail); break;
    case TY_STRING: buf_printf(&b, "  if (!v || v->kind != YU_JV_STRING) %s\n  *out = v->text;\n  return true;\n", fail); break;
    case TY_OPTION:
      buf_printf(&b, "  if (!v || v->kind == YU_JV_NULL) { out->some = false; return true; }\n  out->some = true;\n  return %s(v, &out->val, path, err);\n", jdec_fn(g, t->a));
      break;
    case TY_LIST: {
      const char *et = ctype(g, t->a);
      buf_printf(&b, "  if (!v || v->kind != YU_JV_ARRAY) %s\n  *out = yu_list_new(sizeof(%s));\n  for (int64_t i = 0; i < v->items.len; i++) {\n    %s e;\n    int64_t save = path->len;\n    yu_json_path_push(path, NULL, i);\n    if (!%s(((yu_jv **)v->items.p)[i], &e, path, err)) return false;\n    path->len = save;\n    yu_list_push(out, &e, sizeof(%s));\n  }\n  return true;\n", fail, et, et, jdec_fn(g, t->a), et);
      break;
    }
    case TY_MAP: {
      const char *vt = ctype(g, t->b);
      buf_printf(&b, "  if (!v || v->kind != YU_JV_OBJECT) %s\n  *out = yu_map_new(1, sizeof(%s));\n  for (int64_t i = 0; i < v->items.len; i++) {\n    yu_jkv *kv = &((yu_jkv *)v->items.p)[i];\n    %s val;\n    int64_t save = path->len;\n    if (path->len) yu_sb_cstr(path, \".\");\n    yu_sb_str(path, kv->key);\n    if (!%s(kv->val, &val, path, err)) return false;\n    path->len = save;\n    yu_map_set(out, &kv->key, &val, sizeof(%s));\n  }\n  return true;\n", fail, vt, vt, jdec_fn(g, t->b), vt);
      break;
    }
    case TY_STRUCT: {
      buf_printf(&b, "  if (!v || v->kind != YU_JV_OBJECT) %s\n", fail);
      for (int i = 0; i < t->decl->params.len; i++) {
        Ty *ft = field_ty(g, t, &t->decl->params.items[i]);
        const char *fn = cs(g, t->decl->params.items[i].name);
        buf_printf(&b, "  { int64_t save = path->len; yu_json_path_push(path, \"%s\", 0);\n    if (!%s(yu_jv_get(v, yu_str_lit(\"%s\", %d)), &out->f_%s, path, err)) return false;\n    path->len = save; }\n", fn, jdec_fn(g, ft), fn, (int)strlen(fn), fn);
      }
      buf_puts(&b, "  return true;\n");
      break;
    }
    case TY_ENUM: {
      buf_printf(&b, "  if (!v || v->kind != YU_JV_STRING) %s\n", fail);
      for (int i = 0; i < t->decl->variants.len; i++) {
        const char *vn = cs(g, t->decl->variants.items[i].name);
        buf_printf(&b, "  if (yu_str_eq(v->text, yu_str_lit(\"%s\", %d))) { out->tag = %d; return true; }\n", vn, (int)strlen(vn), i);
      }
      buf_printf(&b, "  %s\n", fail);
      break;
    }
    default: buf_printf(&b, "  (void)v; (void)out; %s\n", fail); break;
  }
  buf_puts(&b, "}\n");
  buf_puts(&g->helpers, b.p);
  buf_free(&b);
  return n;
}

static const char *gen_module_call(G *g, Expr *e) {
  Expr *f = e->a;
  Str mod = f->a->text, fnm = f->text;
  Ty *t = sub(g, e->ty);
  if (str_eq_c(mod, "http")) {
    if (str_eq_c(fnm, "serve")) {
      const char *net = gen_expr(g, e->args.items[0]);
      const char *port = gen_expr(g, e->args.items[1]);
      const char *h = gen_temp(g, e->args.items[2]);
      const char *hh = newtmp(g);
      emit(g, "yu_http_handler %s; memcpy(&%s, &%s, sizeof %s);", hh, hh, h, hh);
      emit(g, "yu_http_serve(%s, %s, %s);", net, port, hh);
      return "YU_UNIT";
    }
    if (str_eq_c(fnm, "response")) { const char *a0 = gen_expr(g, e->args.items[0]); const char *a1 = gen_expr(g, e->args.items[1]); return sfmt(g, "yu_http_response_text(%s, %s)", a0, a1); }
    if (str_eq_c(fnm, "json")) { const char *a0 = gen_expr(g, e->args.items[0]); const char *a1 = gen_expr(g, e->args.items[1]); return sfmt(g, "yu_http_json(%s, %s)", a0, a1); }
    if (str_eq_c(fnm, "request")) { const char *a0 = gen_expr(g, e->args.items[0]); const char *a1 = gen_expr(g, e->args.items[1]); const char *a2 = gen_expr(g, e->args.items[2]); return sfmt(g, "yu_http_request_make(%s, %s, %s)", a0, a1, a2); }
  }
  if (str_eq_c(mod, "json")) {
    if (str_eq_c(fnm, "encode")) {
      Expr *x = e->args.items[0];
      const char *v = gen_expr(g, x);
      const char *sb = newtmp(g);
      emit(g, "yu_sb %s = {0};", sb);
      emit(g, "%s(&%s, %s);", jenc_fn(g, x->ty), sb, v);
      return sfmt(g, "yu_sb_finish(&%s)", sb);
    }
    if (str_eq_c(fnm, "decode")) {
      Ty *T = sub(g, (Ty *)e->type_args.items[0]);
      const char *s = gen_expr(g, e->args.items[0]);
      const char *r = newtmp(g), *root = newtmp(g), *at = newtmp(g);
      const char *jt = ctype(g, t->b);
      emit(g, "%s %s = {0};", ctype(g, t), r);
      emit(g, "yu_jv *%s = NULL; int64_t %s = 0;", root, at);
      emit(g, "if (!yu_json_parse(%s, &%s, &%s)) {", s, root, at);
      g->indent++;
      emit(g, "%s e_ = { .tag = 0 }; e_.u.v_Syntax.f_at = %s; yu_trace_add(&e_.trace_, %s);", jt, at, loc(g, e->line));
      emit(g, "%s.ok = false; %s.err = e_;", r, r);
      g->indent--;
      emit(g, "} else {");
      g->indent++;
      emit(g, "%s out_ = {0}; yu_sb path_ = {0}; yu_json_err err_ = {0};", ctype(g, T));
      emit(g, "if (%s(%s, &out_, &path_, &err_)) { %s.ok = true; %s.val = out_; }", jdec_fn(g, T), root, r, r);
      emit(g, "else { %s e_ = { .tag = 1 }; e_.u.v_Shape.f_path = err_.path; e_.u.v_Shape.f_expected = yu_str_lit(err_.expected, (int64_t)strlen(err_.expected)); yu_trace_add(&e_.trace_, %s); %s.ok = false; %s.err = e_; }", jt, loc(g, e->line), r, r);
      g->indent--;
      emit(g, "}");
      return r;
    }
  }
  emit(g, "/* unsupported module call */");
  return "YU_UNIT";
}

static const char *gen_call(G *g, Expr *e) {
  Expr *f = e->a;
  Ty *t = sub(g, e->ty);
  switch (e->call_kind) {
    case CK_MODULE: return gen_module_call(g, e);
    case CK_PRELUDE: {
      Str n = f->text;
      if (str_eq_c(n, "Some")) return sfmt(g, "(%s){ .some = true, .val = %s }", ctype(g, t), gen_value(g, e->args.items[0]));
      if (str_eq_c(n, "Ok")) return sfmt(g, "(%s){ .ok = true, .val = %s }", ctype(g, t), gen_value(g, e->args.items[0]));
      if (str_eq_c(n, "Err")) {
        if (t->kind == TY_RESULT && t->b->kind == TY_ENUM) {
          const char *ev = newtmp(g);
          emit(g, "%s %s = %s;", ctype(g, t->b), ev, gen_value(g, e->args.items[0]));
          emit(g, "yu_trace_add(&%s.trace_, %s);", ev, loc(g, e->line));
          return sfmt(g, "(%s){ .ok = false, .err = %s }", ctype(g, t), ev);
        }
        return sfmt(g, "(%s){ .ok = false, .err = %s }", ctype(g, t), gen_value(g, e->args.items[0]));
      }
      if (str_eq_c(n, "assert")) {
        const char *c = gen_expr(g, e->args.items[0]);
        emit(g, "yu_assert(%s, %s, \"assert\");", c, loc(g, e->line));
        return "YU_UNIT";
      }
      if (str_eq_c(n, "assert_eq")) {
        Expr *x = e->args.items[0], *y = e->args.items[1];
        const char *a = gen_temp(g, x), *b = gen_temp(g, y);
        Ty *vt = sub(g, x->ty)->kind == TY_ERR || sub(g, x->ty)->kind == TY_NEVER ? sub(g, y->ty) : sub(g, x->ty);
        emit(g, "if (!%s(%s, %s)) { yu_sb l_ = {0}, r_ = {0}; %s(&l_, %s); %s(&r_, %s); yu_assert_eq_fail(%s, yu_sb_finish(&l_), yu_sb_finish(&r_)); }",
             eq_fn(g, vt), a, b, fmt_fn(g, vt), a, fmt_fn(g, vt), b, loc(g, e->line));
        return "YU_UNIT";
      }
      if (str_eq_c(n, "panic")) {
        const char *m = gen_temp(g, e->args.items[0]);
        emit(g, "yu_fail(\"R-PANIC-01\", %s, \"panic: %%.*s\", (int)%s.len, (const char *)%s.p);", loc(g, e->line), m, m);
        return "YU_UNIT";
      }
      break;
    }
    case CK_FN: {
      Decl *d = find_decl(&g->fns, f->text);
      if (d && d->generics.len) {
        TyVec binds = {0};
        for (int i = 0; i < e->type_args.len; i++) { Ty *b = sub(g, (Ty *)e->type_args.items[i]); VEC_PUSH(g->a, &binds, b); }
        const char *cname = sfmt(g, "%s_%s", fn_cname(g, d->name), mangle_binds(g, &binds));
        gen_fn_instance(g, d, NULL, &d->generics, &binds, cname);
        return sfmt(g, "%s(%s)", cname, gen_args(g, e, NULL));
      }
      return sfmt(g, "%s(%s)", fn_cname(g, f->text), gen_args(g, e, NULL));
    }
    case CK_METHOD: {
      Decl *m = e->method;
      Ty *recv = sub(g, f->a->ty);
      const char *self = gen_value(g, f->a);
      if (recv->args.len || m->generics.len) {
        // instance per receiver type arguments and method type arguments
        StrVec names = {0};
        TyVec binds = {0};
        for (int i = 0; i < recv->decl->generics.len; i++) { VEC_PUSH(g->a, &names, recv->decl->generics.items[i]); VEC_PUSH(g->a, &binds, recv->args.items[i]); }
        for (int i = 0; i < m->generics.len; i++) { VEC_PUSH(g->a, &names, m->generics.items[i]); Ty *b = sub(g, (Ty *)e->type_args.items[i]); VEC_PUSH(g->a, &binds, b); }
        const char *cname = sfmt(g, "%s_%s", method_cname(g, recv->name, m->name), mangle_binds(g, &binds));
        gen_fn_instance(g, m, recv, &names, &binds, cname);
        return sfmt(g, "%s(%s)", cname, gen_args(g, e, self));
      }
      return sfmt(g, "%s(%s)", method_cname(g, recv->name, m->name), gen_args(g, e, self));
    }
    case CK_VALUE: {
      const char *c = gen_temp(g, f);
      return sfmt(g, "%s.fn(%s)", c, gen_args(g, e, sfmt(g, "%s.env", c)));
    }
    case CK_STATIC: {
      Str tn = f->a->text, fnm = f->text;
      if (str_eq_c(tn, "Int") && str_eq_c(fnm, "parse")) {
        const char *r = newtmp(g);
        emit(g, "%s %s = {0};", ctype(g, t), r);
        emit(g, "%s.ok = yu_int_parse(%s, &%s.val);", r, gen_expr(g, e->args.items[0]), r);
        return r;
      }
      if (str_eq_c(tn, "Clock")) return sfmt(g, "yu_clock_fixed(%s)", gen_expr(g, e->args.items[0]));
      if (str_eq_c(tn, "Env")) return sfmt(g, "yu_env_of(%s)", gen_expr(g, e->args.items[0]));
      if (str_eq_c(tn, "Log")) return "yu_log_capture()";
      if (str_eq_c(tn, "Store")) {
        Ty *T = t->a;
        const char *v = gen_value(g, e->args.items[0]);
        const char *st = newtmp(g), *prev = newtmp(g), *cp = newtmp(g);
        emit(g, "yu_store *%s = yu_store_new(sizeof(%s));", st, ctype(g, T));
        emit(g, "yu_arena *%s = yu_store_begin(%s);", prev, st);
        if (needs_copy(g, T)) emit(g, "%s %s = %s(%s);", ctype(g, T), cp, copy_fn(g, T), v);
        else emit(g, "%s %s = %s;", ctype(g, T), cp, v);
        emit(g, "yu_store_commit(%s, &%s, %s);", st, cp, prev);
        return st;
      }
      if (str_eq_c(tn, "Float")) return sfmt(g, "((double)%s)", gen_expr(g, e->args.items[0]));
      if (str_eq_c(tn, "String")) {
        Expr *x = e->args.items[0];
        const char *sb = newtmp(g);
        emit(g, "yu_sb %s = {0};", sb);
        const char *v = gen_expr(g, x);
        if (sub(g, x->ty)->kind == TY_INT) emit(g, "yu_sb_int(&%s, %s);", sb, v);
        else if (sub(g, x->ty)->kind == TY_FLOAT) emit(g, "yu_sb_float(&%s, %s);", sb, v);
        else emit(g, "yu_sb_bool(&%s, %s);", sb, v);
        return sfmt(g, "yu_sb_finish(&%s)", sb);
      }
      break;
    }
    case CK_VARIANT: {
      Str vn = f->text;
      const char *r = newtmp(g);
      emit(g, "%s %s = { .tag = yu_%s_%s };", ctype(g, t), r, mangle(g, t), cs(g, vn));
      for (int i = 0; i < e->args.len; i++) {
        Str fname = e->arg_names.items[i];
        emit(g, "%s.u.v_%s.f_%s = %s;", r, cs(g, vn), cs(g, fname), gen_value(g, e->args.items[i]));
      }
      return r;
    }
    case CK_BUILTIN: return gen_builtin(g, e);
    default: break;
  }
  emit(g, "/* unsupported call */");
  return "YU_UNIT";
}

// ---- statements --------------------------------------------------------

static void gen_stmts(G *g, Block *b) {
  for (int i = 0; i < b->stmts.len; i++) {
    Stmt *s = b->stmts.items[i];
    switch (s->kind) {
      case S_LET: {
        if (str_eq_c(s->name, "_")) { const char *v = gen_expr(g, s->value); emit(g, "(void)(%s);", v); break; }
        if (sub(g, s->ty)->kind == TY_UNIT || sub(g, s->ty)->kind == TY_NEVER) {
          const char *v = gen_expr(g, s->value);
          emit(g, "(void)(%s);", v);
          const char *cn = bind_new(g, s->name);
          emit(g, "yu_unit %s = YU_UNIT; (void)%s;", cn, cn);
          break;
        }
        const char *v = gen_value(g, s->value);  // generated before the new name shadows an outer one
        const char *cn = bind_new(g, s->name);
        emit(g, "%s %s = %s;", ctype(g, s->ty), cn, v);
        break;
      }
      case S_ASSIGN: {
        const char *v = gen_value(g, s->value);
        const char *p = gen_place(g, s->target);
        emit(g, "%s = %s;", p, v);
        break;
      }
      case S_EXPR: {
        const char *v = gen_expr(g, s->value);
        if (s->value->kind != E_IF && s->value->kind != E_MATCH && s->value->kind != E_BLOCK) emit(g, "(void)(%s);", v);
        break;
      }
      case S_RETURN:
        if (s->value) {
          if (sub(g, g->fn_ret)->kind == TY_UNIT) { const char *v = gen_expr(g, s->value); emit(g, "(void)(%s);", v); emit(g, "return YU_UNIT;"); }
          else { const char *v = gen_value(g, s->value); emit(g, "return %s;", v); }
        } else emit(g, "return YU_UNIT;");
        break;
      case S_BREAK: emit(g, "break;"); break;
      case S_CONTINUE: emit(g, "continue;"); break;
      case S_FOR: {
        Expr *it = s->value;
        int loop_mark = scope_push(g);
        if (it->kind == E_BINARY && it->op == T_DOTDOT) {
          const char *lo = gen_temp(g, it->a), *hi = gen_temp(g, it->b);
          const char *i = newtmp(g);
          emit(g, "for (int64_t %s = %s; %s < %s; %s++) {", i, lo, i, hi, i);
          g->indent++;
          const char *cn = bind_new(g, s->name);
          emit(g, "int64_t %s = %s; (void)%s;", cn, i, cn);
        } else {
          const char *l = gen_temp(g, it);
          const char *i = newtmp(g);
          emit(g, "for (int64_t %s = 0; %s < %s.len; %s++) {", i, i, l, i);
          g->indent++;
          const char *cn = bind_new(g, s->name);
          emit(g, "%s %s = ((%s *)%s.p)[%s]; (void)%s;", ctype(g, s->ty), cn, ctype(g, s->ty), l, i, cn);
        }
        gen_block_into(g, s->body, NULL, NULL);
        g->indent--;
        emit(g, "}");
        scope_pop(g, loop_mark);
        break;
      }
      case S_WHILE: {
        emit(g, "for (;;) {");
        g->indent++;
        const char *c = gen_expr(g, s->value);
        emit(g, "if (!(%s)) break;", c);
        gen_block_into(g, s->body, NULL, NULL);
        g->indent--;
        emit(g, "}");
        break;
      }
    }
  }
}

// ---- declarations ------------------------------------------------------

static void gen_fn_named(G *g, Decl *d, const char *name);

static void gen_fn_decl(G *g, Decl *d, Ty *self_type) {
  gen_fn_named(g, d, self_type ? method_cname(g, self_type->name, d->name) : fn_cname(g, d->name));
}

// Generates one instance of a generic function or method under the substitution names -> binds.
static void gen_fn_instance(G *g, Decl *d, Ty *self_type, StrVec *names, TyVec *binds, const char *cname) {
  (void)self_type;
  if (seen(&g->instances, cname)) return;
  mark(g, &g->instances, cname);
  Buf *saved_out = g->out;
  Ty *saved_ret = g->fn_ret;
  int saved_indent = g->indent;
  StrVec *saved_names = g->sub_names;
  TyVec *saved_binds = g->sub_binds;
  g->sub_names = names;
  g->sub_binds = binds;
  gen_fn_named(g, d, cname);
  g->sub_names = saved_names;
  g->sub_binds = saved_binds;
  g->out = saved_out;
  g->fn_ret = saved_ret;
  g->indent = saved_indent;
}

static void gen_fn_named(G *g, Decl *d, const char *name) {
  Ty *ft = sub(g, d->ty);
  Buf sig = {0};
  int fn_mark = scope_push(g);
  buf_printf(&sig, "static %s %s(", ctype(g, ft->ret), name);
  const char **pnames = arena_alloc(g->a, sizeof(char *) * (size_t)(d->params.len + 1));
  for (int i = 0; i < d->params.len; i++) {
    if (i) buf_puts(&sig, ", ");
    pnames[i] = bind_new(g, d->params.items[i].name);
    buf_printf(&sig, "%s %s", ctype(g, d->params.items[i].ty), pnames[i]);
  }
  if (d->params.len == 0) buf_puts(&sig, "void");
  buf_puts(&sig, ")");
  buf_printf(&g->protos, "%s;\n", sig.p);
  Buf body = {0};
  Buf *outer_out = g->out;  // non-NULL while generating an instance from inside another body
  g->out = &body;
  g->fn_ret = ft->ret;
  g->indent = 1;
  for (int i = 0; i < d->params.len; i++) emit(g, "(void)%s;", pnames[i]);
  if (ft->ret->kind == TY_UNIT) { gen_block_into(g, d->body, NULL, NULL); emit(g, "return YU_UNIT;"); }
  else {
    const char *r = newtmp(g);
    emit(g, "%s %s = {0};", ctype(g, ft->ret), r);
    gen_block_into(g, d->body, r, ft->ret);
    emit(g, "return %s;", r);
  }
  buf_printf(&g->code, "%s {\n%s}\n\n", sig.p, body.p);
  buf_free(&sig);
  buf_free(&body);
  scope_pop(g, fn_mark);
  g->out = outer_out;
}

void cgen_module(Arena *a, Module *m, bool tests, Buf *out) {
  G gg = {.a = a, .m = m, .file = m->file};
  G *g = &gg;
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    if (d->kind == D_STRUCT) VEC_PUSH(a, &g->structs, d);
    if (d->kind == D_ENUM) VEC_PUSH(a, &g->enums, d);
    if (d->kind == D_CONST) VEC_PUSH(a, &g->consts, d);
    if (d->kind == D_FN) VEC_PUSH(a, &g->fns, d);
  }
  // all user types first, so the order of use does not matter
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    if ((d->kind == D_STRUCT || d->kind == D_ENUM) && d->generics.len == 0)
      emit_type(g, ty_named(a, d->kind == D_STRUCT ? TY_STRUCT : TY_ENUM, d->name, d));
  }
  // constants
  Buf consts = {0};
  for (int i = 0; i < g->consts.len; i++) {
    Decl *d = g->consts.items[i];
    Expr *v = d->value;
    if (v->kind == E_STRING) {
      int64_t n;
      const char *body = c_string(g, v->segs.items[0], &n);
      buf_printf(&consts, "static const yu_str %s = {(const uint8_t *)\"%s\", %lld};\n", fn_cname(g, d->name), body, (long long)n);
    } else if (v->kind == E_INT) buf_printf(&consts, "static const int64_t %s = INT64_C(%s);\n", fn_cname(g, d->name), cs(g, v->text));
    else if (v->kind == E_FLOAT) buf_printf(&consts, "static const double %s = %s;\n", fn_cname(g, d->name), cs(g, v->text));
    else buf_printf(&consts, "static const bool %s = %s;\n", fn_cname(g, d->name), v->bool_val ? "true" : "false");
  }
  // functions, methods, tests
  Buf test_table = {0};
  int ntests = 0;
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    if (d->kind == D_FN && d->generics.len == 0) gen_fn_decl(g, d, NULL);
    else if (d->kind == D_IMPL) {
      Decl *sd = find_decl(&g->structs, d->name);
      Decl *ed = sd ? NULL : find_decl(&g->enums, d->name);
      if (!sd && !ed) continue;
      Ty *self_type = ty_named(a, sd ? TY_STRUCT : TY_ENUM, d->name, sd ? sd : ed);
      for (int j = 0; j < d->methods.len; j++) if (d->methods.items[j]->generics.len == 0) gen_fn_decl(g, d->methods.items[j], self_type);
    } else if (d->kind == D_TEST) {
      Buf body = {0};
      g->out = &body;
      g->fn_ret = ty_prim(TY_UNIT);
      g->indent = 1;
      gen_block_into(g, d->body, NULL, NULL);
      buf_printf(&g->protos, "static void yu_test_%d(void);\n", ntests);
      buf_printf(&g->code, "static void yu_test_%d(void) {\n%s}\n\n", ntests, body.p);
      int64_t n;
      const char *name = c_string(g, d->name, &n);
      buf_printf(&test_table, "  yu_test_run(\"%s\", yu_test_%d);\n", name, ntests);
      ntests++;
      buf_free(&body);
      g->out = NULL;
    }
  }
  // assemble
  buf_puts(out, "// generated by yuseri stage0; do not edit\n#include \"yu_rt.h\"\n#include <math.h>\n#include <stddef.h>\n\n");
  buf_write(out, g->types.p ? g->types.p : "", g->types.len);
  buf_puts(out, "\n");
  buf_write(out, consts.p ? consts.p : "", consts.len);
  buf_puts(out, "\n");
  buf_write(out, g->protos.p ? g->protos.p : "", g->protos.len);
  buf_puts(out, "\n");
  buf_write(out, g->helpers.p ? g->helpers.p : "", g->helpers.len);
  buf_puts(out, "\n");
  buf_write(out, g->code.p ? g->code.p : "", g->code.len);
  if (tests) {
    buf_puts(out, "int main(void) {\n  yu_init();\n");
    buf_write(out, test_table.p ? test_table.p : "", test_table.len);
    buf_puts(out, "  return yu_test_summary();\n}\n");
  } else {
    Decl *mainfn = find_decl(&g->fns, STR_LIT("main"));
    if (mainfn && mainfn->params.len == 1) buf_puts(out, "int main(void) {\n  yu_init();\n  yu_system sys = yu_system_new();\n  (void)y_main(sys);\n  return 0;\n}\n");
    else if (mainfn) buf_puts(out, "int main(void) {\n  yu_init();\n  (void)y_main();\n  return 0;\n}\n");
    else buf_puts(out, "int main(void) {\n  fprintf(stderr, \"yuseri: this module has no main; run its tests with `yuseri test`\\n\");\n  return 2;\n}\n");
  }
  buf_free(&consts);
  buf_free(&test_table);
  buf_free(&g->types);
  buf_free(&g->protos);
  buf_free(&g->helpers);
  buf_free(&g->code);
}
