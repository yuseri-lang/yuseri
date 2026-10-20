#include "check.h"

#include "lexer.h"
#include "parser.h"
#include "types.h"

// Declarations that `import std.json` / `import std.http` bring into the module.
static const char *STD_JSON_SRC =
    "enum JsonError { Syntax(at: Int), Shape(path: String, expected: String) }\n";
static const char *STD_HTTP_SRC =
    "struct Request { id: String, method: String, path: String, query: Map<String, String>, headers: Map<String, String>, body: String }\n"
    "struct Response { status: Int, headers: Map<String, String>, body: String }\n";

// ---- context -----------------------------------------------------------

typedef struct Sym {
  Str name;
  Ty *type;
  bool mutable, used, is_param, is_self;
  int line;
  int fn_depth;  // nesting depth of the function that declared it
} Sym;

typedef VEC(Sym *) SymVec;

typedef struct Scope {
  SymVec syms;
  struct Scope *parent;
} Scope;

typedef struct FnCtx {
  Ty *ret;
  Str name;
  int depth;
  struct FnCtx *outer;
  Expr *fn_expr;  // the E_FN for an anonymous function, NULL for a declaration
} FnCtx;

typedef struct {
  Arena *a;
  const char *file;
  Module *m;
  DeclVec fns, structs, enums, consts, impls;
  StrVec modules;   // imported std modules
  Scope *scope;
  FnCtx *fn;
  StrVec *generics;  // generic parameter names of the current function
  StrVec *outer_generics;  // generic parameter names of the impl target, inside its methods
  int loop_depth;    // > 0 inside a for / while body of the current function
} C;

#define ERR(c, id, line, ...) diag_error(id, (c)->file, line, __VA_ARGS__)

static Ty *infer(C *c, Expr *e, Ty *expected);
static bool json_able(C *c, Ty *t);
static Ty *check_expr(C *c, Expr *e, Ty *expected);
static Ty *check_block(C *c, Block *b, Ty *expected);
static void check_stmt(C *c, Stmt *s);

static const char *tstr(C *c, Ty *t) { return ty_str(c->a, t); }
static char *cstr(C *c, Str s) { return arena_cstr(c->a, s); }

// ---- scopes ------------------------------------------------------------

static void push_scope(C *c) {
  Scope *s = NEW(c->a, Scope);
  s->parent = c->scope;
  c->scope = s;
}

static void pop_scope(C *c) {
  Scope *s = c->scope;
  for (int i = 0; i < s->syms.len; i++) {
    Sym *y = s->syms.items[i];
    if (!y->used && !y->is_self && !str_eq_c(y->name, "_"))
      ERR(c, "E-BIND-01", y->line, "unused %s %s", y->is_param ? "parameter" : "binding", cstr(c, y->name));
  }
  c->scope = s->parent;
}

static Sym *define(C *c, Str name, Ty *type, bool mutable, int line, bool is_param) {
  if (str_eq_c(name, "_")) return NULL;
  for (int i = 0; i < c->scope->syms.len; i++)
    if (str_eq(c->scope->syms.items[i]->name, name)) {
      ERR(c, "E-NAME-02", line, "duplicate definition of %s", cstr(c, name));
      return c->scope->syms.items[i];
    }
  Sym *y = NEW(c->a, Sym);
  y->name = name;
  y->type = type;
  y->mutable = mutable;
  y->line = line;
  y->is_param = is_param;
  y->is_self = str_eq_c(name, "self");
  y->fn_depth = c->fn ? c->fn->depth : 0;
  VEC_PUSH(c->a, &c->scope->syms, y);
  return y;
}

static Sym *find_sym(C *c, Str name) {
  for (Scope *s = c->scope; s; s = s->parent)
    for (int i = s->syms.len - 1; i >= 0; i--)
      if (str_eq(s->syms.items[i]->name, name)) return s->syms.items[i];
  return NULL;
}

// Resolves a local name for reading; reports captured `var`s inside anonymous functions.
static void record_capture(C *c, Expr *fn, Sym *y) {
  for (int i = 0; i < fn->cap_names.len; i++) if (str_eq(fn->cap_names.items[i], y->name)) return;
  VEC_PUSH(c->a, &fn->cap_names, y->name);
  VEC_PUSH(c->a, &fn->cap_types, y->type);
}

static Sym *use_sym(C *c, Str name, int line) {
  Sym *y = find_sym(c, name);
  if (!y) return NULL;
  y->used = true;
  if (c->fn && y->fn_depth < c->fn->depth) {
    if (y->mutable) ERR(c, "E-BIND-03", line, "anonymous function captures var %s", cstr(c, name));
    // every anonymous function between the use and the declaring function captures it
    for (FnCtx *f = c->fn; f && f->depth > y->fn_depth; f = f->outer)
      if (f->fn_expr) record_capture(c, f->fn_expr, y);
  }
  return y;
}

// ---- declarations lookup -----------------------------------------------

static Decl *find_decl(DeclVec *v, Str name) {
  for (int i = 0; i < v->len; i++) if (str_eq(v->items[i]->name, name)) return v->items[i];
  return NULL;
}

static Decl *find_method(C *c, Str type_name, Str method) {
  for (int i = 0; i < c->impls.len; i++) {
    Decl *im = c->impls.items[i];
    if (!str_eq(im->name, type_name)) continue;
    Decl *m = find_decl(&im->methods, method);
    if (m) return m;
  }
  return NULL;
}

static bool is_generic_name(C *c, Str name) {
  if (c->generics)
    for (int i = 0; i < c->generics->len; i++) if (str_eq(c->generics->items[i], name)) return true;
  if (c->outer_generics)
    for (int i = 0; i < c->outer_generics->len; i++) if (str_eq(c->outer_generics->items[i], name)) return true;
  return false;
}

static bool is_type_name(C *c, Str n) {
  static const char *builtins[] = {"Int", "Float", "Bool", "String", "Bytes", "Unit", "List", "Map", "Option", "Result", "ParseError"};
  for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) if (str_eq_c(n, builtins[i])) return true;
  return find_decl(&c->structs, n) || find_decl(&c->enums, n);
}

// ---- types from syntax -------------------------------------------------

static Ty *resolve_type(C *c, Type *t) {
  if (!t) return ty_prim(TY_ERR);
  if (t->kind == TYPE_FN) {
    TyVec ps = {0};
    for (int i = 0; i < t->args.len; i++) { Ty *p = resolve_type(c, t->args.items[i]); VEC_PUSH(c->a, &ps, p); }
    return ty_fn(c->a, ps, resolve_type(c, t->ret));
  }
  Str n = t->name;
  int nargs = t->args.len;
  Ty *a0 = nargs > 0 ? resolve_type(c, t->args.items[0]) : NULL;
  Ty *a1 = nargs > 1 ? resolve_type(c, t->args.items[1]) : NULL;
#define PRIM(lit, k) if (str_eq_c(n, lit)) { if (nargs) ERR(c, "E-TYPE-06", t->line, "%s takes no type arguments", lit); return ty_prim(k); }
  PRIM("Int", TY_INT) PRIM("Float", TY_FLOAT) PRIM("Bool", TY_BOOL) PRIM("String", TY_STRING)
  PRIM("Bytes", TY_BYTES) PRIM("Unit", TY_UNIT)
#undef PRIM
  if (str_eq_c(n, "List")) { if (nargs != 1) { ERR(c, "E-TYPE-06", t->line, "List takes 1 type argument"); return ty_prim(TY_ERR); } return ty_list(c->a, a0); }
  if (str_eq_c(n, "Option")) { if (nargs != 1) { ERR(c, "E-TYPE-06", t->line, "Option takes 1 type argument"); return ty_prim(TY_ERR); } return ty_option(c->a, a0); }
  if (str_eq_c(n, "Map")) { if (nargs != 2) { ERR(c, "E-TYPE-06", t->line, "Map takes 2 type arguments"); return ty_prim(TY_ERR); } return ty_map(c->a, a0, a1); }
  if (str_eq_c(n, "Result")) {
    if (nargs != 2) { ERR(c, "E-TYPE-06", t->line, "Result takes 2 type arguments"); return ty_prim(TY_ERR); }
    if (a1 && a1->kind != TY_ENUM && a1->kind != TY_PARAM && a1->kind != TY_ERR) ERR(c, "E-TYPE-01", t->line, "the error type of Result must be an enum, got %s", tstr(c, a1));
    return ty_result(c->a, a0, a1);
  }
  if (str_eq_c(n, "ParseError")) return ty_named(c->a, TY_ENUM, n, NULL);
  if (is_generic_name(c, n)) return ty_param(c->a, n);
  Decl *d = find_decl(&c->structs, n);
  TyKind k = TY_STRUCT;
  if (!d) { d = find_decl(&c->enums, n); k = TY_ENUM; }
  if (!d) { ERR(c, "E-NAME-01", t->line, "unknown type %s", cstr(c, n)); return ty_prim(TY_ERR); }
  Ty *ty = ty_named(c->a, k, n, d);
  if (nargs != d->generics.len) { ERR(c, "E-TYPE-06", t->line, "%s takes %d type arguments, got %d", cstr(c, n), d->generics.len, nargs); return ty_prim(TY_ERR); }
  for (int i = 0; i < nargs; i++) { Ty *x = resolve_type(c, t->args.items[i]); VEC_PUSH(c->a, &ty->args, x); }
  return ty;
}

// Field type of a struct instance (generic arguments substituted). NULL if no such field.
static Ty *struct_field(C *c, Ty *st, Str field) {
  Decl *d = st->decl;
  for (int i = 0; i < d->params.len; i++)
    if (str_eq(d->params.items[i].name, field)) {
      StrVec *saved = c->generics;
      c->generics = &d->generics;
      Ty *ft = resolve_type(c, d->params.items[i].type);
      c->generics = saved;
      return ty_subst(c->a, ft, &d->generics, &st->args);
    }
  return NULL;
}

static Variant *enum_variant(Decl *d, Str name) {
  for (int i = 0; i < d->variants.len; i++) if (str_eq(d->variants.items[i].name, name)) return &d->variants.items[i];
  return NULL;
}

static Ty *variant_field_type(C *c, Ty *et, Variant *v, Str field) {
  for (int i = 0; i < v->fields.len; i++)
    if (str_eq(v->fields.items[i].name, field)) {
      StrVec *saved = c->generics;
      c->generics = &et->decl->generics;
      Ty *ft = resolve_type(c, v->fields.items[i].type);
      c->generics = saved;
      return ty_subst(c->a, ft, &et->decl->generics, &et->args);
    }
  return NULL;
}

// Type of a top-level function as a value (generic parameters stay as TY_PARAM).
static Ty *fn_type(C *c, Decl *d, Ty *self_type) {
  StrVec *saved = c->generics;
  StrVec *saved_outer = c->outer_generics;
  c->generics = &d->generics;
  if (self_type && self_type->decl && self_type->decl->generics.len) c->outer_generics = &self_type->decl->generics;
  TyVec ps = {0};
  for (int i = 0; i < d->params.len; i++) {
    Param *p = &d->params.items[i];
    Ty *t = p->type ? resolve_type(c, p->type) : (self_type ? self_type : ty_prim(TY_ERR));
    VEC_PUSH(c->a, &ps, t);
  }
  Ty *ret = d->ret ? resolve_type(c, d->ret) : ty_prim(TY_ERR);
  c->generics = saved;
  c->outer_generics = saved_outer;
  return ty_fn(c->a, ps, ret);
}

// ---- helpers for expressions -------------------------------------------

static const char *callee_name(C *c, Expr *e) {
  if (e->kind != E_CALL) return "expression";
  Expr *f = e->a;
  if (f->kind == E_IDENT) return cstr(c, f->text);
  if (f->kind == E_FIELD) return cstr(c, f->text);
  return "call";
}

// `None`, `Ok(..)`, `Err(..)`, `[]` take their type from context.
static bool is_contextual(Expr *e) {
  if (e->kind == E_IDENT) return str_eq_c(e->text, "None");
  if (e->kind == E_LIST) return e->args.len == 0;
  if (e->kind == E_CALL && e->a->kind == E_IDENT) return str_eq_c(e->a->text, "Ok") || str_eq_c(e->a->text, "Err");
  if (e->kind == E_FIELD && e->a->kind == E_IDENT) return true;  // may be a unit variant of a generic enum
  return false;
}

static bool is_mutable_place(C *c, Expr *e) {
  if (e->kind == E_IDENT) { Sym *y = find_sym(c, e->text); return y && y->mutable; }
  if (e->kind == E_FIELD || e->kind == E_INDEX) return is_mutable_place(c, e->a);
  return false;
}

static const char *place_name(C *c, Expr *e) {
  while (e->kind == E_FIELD || e->kind == E_INDEX) e = e->a;
  return e->kind == E_IDENT ? cstr(c, e->text) : "value";
}

static void check_arg_count(C *c, Expr *call, int want, const char *what) {
  if (call->args.len != want) ERR(c, "E-TYPE-06", call->line, "%s takes %d argument%s, got %d", what, want, want == 1 ? "" : "s", call->args.len);
}

static Ty *arg_or_err(C *c, Expr *call, int i, Ty *expected) {
  if (i >= call->args.len) return ty_prim(TY_ERR);
  return check_expr(c, call->args.items[i], expected);
}

// Calls a user function or a function value: checks arguments, binds generics.
static Ty *call_fn_type(C *c, Expr *call, Ty *ft, StrVec *gnames, const char *what, Ty *expected) {
  if (ft->params.len != call->args.len) {
    ERR(c, "E-TYPE-06", call->line, "%s takes %d argument%s, got %d", what, ft->params.len, ft->params.len == 1 ? "" : "s", call->args.len);
    return ty_prim(TY_ERR);
  }
  TyVec binds = {0};
  if (gnames && expected && !ty_is_err(expected) && ty_has_param(ft->ret)) ty_unify(c->a, ft->ret, expected, gnames, &binds);
  // two passes: arguments whose type is their own first, then the ones that need context
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < ft->params.len; i++) {
      Ty *pt = ft->params.items[i];
      Expr *arg = call->args.items[i];
      bool contextual = gnames && ty_has_param(pt) && is_contextual(arg);
      if ((pass == 0) == contextual) continue;
      if (pass == 0 && call->arg_names.len > i && call->arg_names.items[i].len)
        ERR(c, "E-TYPE-06", arg->line, "%s does not take named arguments", what);
      if (gnames && ty_has_param(pt)) {
        Ty *known = ty_subst(c->a, pt, gnames, &binds);
        Ty *at = infer(c, arg, ty_has_param(known) ? NULL : known);
        if (!ty_unify(c->a, pt, at, gnames, &binds))
          ERR(c, "E-TYPE-01", arg->line, "expected %s, got %s", tstr(c, known), tstr(c, at));
      } else {
        check_expr(c, arg, pt);
      }
    }
  }
  if (!gnames) return ft->ret;
  for (int i = 0; i < gnames->len; i++) {
    Ty *b = i < binds.len ? binds.items[i] : NULL;
    if (!b) { ERR(c, "E-TYPE-05", call->line, "cannot infer %s for %s", cstr(c, gnames->items[i]), what); b = ty_prim(TY_ERR); }
    VEC_PUSH(c->a, &call->type_args, b);
  }
  return ty_subst(c->a, ft->ret, gnames, &binds);
}

// ---- builtin methods ---------------------------------------------------

static bool method_is(Str m, const char *name) { return str_eq_c(m, name); }

// Returns NULL when `recv` has no builtin method `name`.
static Ty *builtin_method(C *c, Ty *recv, Str name, Expr *call, Ty *expected, bool *found) {
  *found = true;
  Arena *a = c->a;
  Ty *INT = ty_prim(TY_INT), *BOOL = ty_prim(TY_BOOL), *STRING = ty_prim(TY_STRING), *UNIT = ty_prim(TY_UNIT);
  Expr *obj = call->a->a;
  bool needs_var = false;
  Ty *result = NULL;
  switch (recv->kind) {
    case TY_LIST: {
      Ty *T = recv->a;
      if (method_is(name, "push")) { needs_var = true; check_arg_count(c, call, 1, "push"); arg_or_err(c, call, 0, T); result = UNIT; }
      else if (method_is(name, "get")) { check_arg_count(c, call, 1, "get"); arg_or_err(c, call, 0, INT); result = ty_option(a, T); }
      else if (method_is(name, "len")) { check_arg_count(c, call, 0, "len"); result = INT; }
      else if (method_is(name, "contains")) { check_arg_count(c, call, 1, "contains"); arg_or_err(c, call, 0, T); result = BOOL; }
      else if (method_is(name, "map") || method_is(name, "filter")) {
        check_arg_count(c, call, 1, cstr(c, name));
        Ty *ft = arg_or_err(c, call, 0, NULL);
        if (ft->kind == TY_FN) {
          if (ft->params.len != 1 || !ty_eq(ft->params.items[0], T))
            ERR(c, "E-TYPE-01", call->line, "%s needs fn(%s) -> ..., got %s", cstr(c, name), tstr(c, T), tstr(c, ft));
          if (method_is(name, "filter") && !ty_eq(ft->ret, BOOL))
            ERR(c, "E-TYPE-01", call->line, "filter needs fn(%s) -> Bool, got %s", tstr(c, T), tstr(c, ft));
          result = method_is(name, "map") ? ty_list(a, ft->ret) : recv;
        } else if (!ty_is_err(ft)) {
          ERR(c, "E-TYPE-01", call->line, "%s needs a function, got %s", cstr(c, name), tstr(c, ft));
          result = ty_prim(TY_ERR);
        } else result = ty_prim(TY_ERR);
      } else *found = false;
      break;
    }
    case TY_MAP: {
      Ty *K = recv->a, *V = recv->b;
      if (method_is(name, "set")) { needs_var = true; check_arg_count(c, call, 2, "set"); arg_or_err(c, call, 0, K); arg_or_err(c, call, 1, V); result = UNIT; }
      else if (method_is(name, "get")) { check_arg_count(c, call, 1, "get"); arg_or_err(c, call, 0, K); result = ty_option(a, V); }
      else if (method_is(name, "has")) { check_arg_count(c, call, 1, "has"); arg_or_err(c, call, 0, K); result = BOOL; }
      else if (method_is(name, "remove")) { needs_var = true; check_arg_count(c, call, 1, "remove"); arg_or_err(c, call, 0, K); result = UNIT; }
      else if (method_is(name, "len")) { check_arg_count(c, call, 0, "len"); result = INT; }
      else if (method_is(name, "entries")) { check_arg_count(c, call, 0, "entries"); result = ty_list(a, ty_entry(a, K, V)); }
      else *found = false;
      break;
    }
    case TY_OPTION: {
      Ty *T = recv->a;
      if (method_is(name, "is_some")) { check_arg_count(c, call, 0, "is_some"); result = BOOL; }
      else if (method_is(name, "unwrap_or")) { check_arg_count(c, call, 1, "unwrap_or"); arg_or_err(c, call, 0, T); result = T; }
      else if (method_is(name, "ok_or")) {
        check_arg_count(c, call, 1, "ok_or");
        Ty *E = call->args.len ? infer(c, call->args.items[0], expected && expected->kind == TY_RESULT ? expected->b : NULL) : ty_prim(TY_ERR);
        result = ty_result(a, T, E);
      } else *found = false;
      break;
    }
    case TY_RESULT: {
      if (method_is(name, "is_ok")) { check_arg_count(c, call, 0, "is_ok"); result = BOOL; }
      else if (method_is(name, "unwrap_or")) { check_arg_count(c, call, 1, "unwrap_or"); arg_or_err(c, call, 0, recv->a); result = recv->a; }
      else *found = false;
      break;
    }
    case TY_ENUM:
      if (method_is(name, "trace")) { check_arg_count(c, call, 0, "trace"); result = ty_list(a, STRING); }
      else *found = false;
      break;
    case TY_STRING: {
      struct { const char *n; int nargs; TyKind a0, a1; int ret; } table[] = {
          // ret: 0 Int, 1 Bool, 2 String, 3 List<String>, 4 Option<Int>, 5 Bytes
          {"len", 0, 0, 0, 0}, {"is_empty", 0, 0, 0, 1}, {"split", 1, TY_STRING, 0, 3}, {"trim", 0, 0, 0, 2},
          {"starts_with", 1, TY_STRING, 0, 1}, {"ends_with", 1, TY_STRING, 0, 1}, {"contains", 1, TY_STRING, 0, 1},
          {"find", 1, TY_STRING, 0, 4}, {"replace", 2, TY_STRING, TY_STRING, 2}, {"to_upper", 0, 0, 0, 2},
          {"to_lower", 0, 0, 0, 2}, {"slice", 2, TY_INT, TY_INT, 2}, {"chars", 0, 0, 0, 3}, {"bytes", 0, 0, 0, 5},
          {"repeat", 1, TY_INT, 0, 2},
      };
      bool hit = false;
      for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (!method_is(name, table[i].n)) continue;
        hit = true;
        check_arg_count(c, call, table[i].nargs, table[i].n);
        if (table[i].nargs > 0) arg_or_err(c, call, 0, ty_prim(table[i].a0));
        if (table[i].nargs > 1) arg_or_err(c, call, 1, ty_prim(table[i].a1));
        switch (table[i].ret) {
          case 0: result = INT; break;
          case 1: result = BOOL; break;
          case 2: result = STRING; break;
          case 3: result = ty_list(a, STRING); break;
          case 4: result = ty_option(a, INT); break;
          default: result = ty_prim(TY_BYTES); break;
        }
      }
      if (!hit && method_is(name, "join")) { hit = true; check_arg_count(c, call, 1, "join"); arg_or_err(c, call, 0, ty_list(a, STRING)); result = STRING; }
      if (!hit) *found = false;
      break;
    }
    case TY_ENTRY: *found = false; break;
    default: *found = false; break;
  }
  if (!*found) return NULL;
  if (needs_var && !is_mutable_place(c, obj))
    ERR(c, "E-BIND-02", call->line, "%s on let %s", cstr(c, name), place_name(c, obj));
  return result;
}

// ---- patterns and exhaustiveness ---------------------------------------

static bool pat_irrefutable(Pattern *p) {
  if (p->kind == PAT_WILD || p->kind == PAT_BIND) return true;
  return false;
}

static void check_pattern(C *c, Pattern *p, Ty *t) {
  p->ty = t;
  switch (p->kind) {
    case PAT_WILD: return;
    case PAT_BIND: p->ty = t; define(c, p->name, t, false, p->line, false); return;
    case PAT_LIT: {
      Ty *lt = infer(c, p->lit, NULL);
      if (!ty_eq(lt, t)) ERR(c, "E-TYPE-01", p->line, "pattern has type %s, value has type %s", tstr(c, lt), tstr(c, t));
      return;
    }
    case PAT_VARIANT: break;
  }
  if (ty_is_err(t)) {
    for (int i = 0; i < p->fields.len; i++) check_pattern(c, p->fields.items[i].pat, ty_prim(TY_ERR));
    return;
  }
  Str head = p->path.items[0];
  if (p->path.len == 1) {
    const char *want = NULL;
    Ty *inner = NULL;
    bool unit = false;
    if (str_eq_c(head, "None")) { want = "Option"; unit = true; if (t->kind == TY_OPTION) inner = NULL; }
    else if (str_eq_c(head, "Some")) { want = "Option"; if (t->kind == TY_OPTION) inner = t->a; }
    else if (str_eq_c(head, "Ok")) { want = "Result"; if (t->kind == TY_RESULT) inner = t->a; }
    else if (str_eq_c(head, "Err")) { want = "Result"; if (t->kind == TY_RESULT) inner = t->b; }
    if (!want) { ERR(c, "E-NAME-01", p->line, "unknown pattern %s; enum variants are written Type.Variant", cstr(c, head)); return; }
    bool kind_ok = (strcmp(want, "Option") == 0 && t->kind == TY_OPTION) || (strcmp(want, "Result") == 0 && t->kind == TY_RESULT);
    if (!kind_ok) { ERR(c, "E-TYPE-01", p->line, "pattern %s needs %s, value has type %s", cstr(c, head), want, tstr(c, t)); return; }
    if (unit) { if (p->has_parens) ERR(c, "E-TYPE-06", p->line, "None takes no fields"); return; }
    if (p->fields.len != 1) { ERR(c, "E-TYPE-06", p->line, "%s takes 1 field, got %d", cstr(c, head), p->fields.len); return; }
    check_pattern(c, p->fields.items[0].pat, inner);
    return;
  }
  if (p->path.len != 2) { ERR(c, "E-NAME-01", p->line, "unknown pattern"); return; }
  if (t->kind != TY_ENUM || !t->decl) { ERR(c, "E-TYPE-01", p->line, "pattern %s.%s needs an enum, value has type %s", cstr(c, head), cstr(c, p->path.items[1]), tstr(c, t)); return; }
  if (!str_eq(t->name, head)) { ERR(c, "E-TYPE-01", p->line, "pattern %s.%s does not match type %s", cstr(c, head), cstr(c, p->path.items[1]), tstr(c, t)); return; }
  Variant *v = enum_variant(t->decl, p->path.items[1]);
  if (!v) { ERR(c, "E-NAME-01", p->line, "%s has no variant %s", cstr(c, head), cstr(c, p->path.items[1])); return; }
  for (int i = 0; i < p->fields.len; i++) {
    PatField *f = &p->fields.items[i];
    Ty *ft;
    if (f->name.len) {
      ft = variant_field_type(c, t, v, f->name);
      if (!ft) { ERR(c, "E-TYPE-06", p->line, "%s.%s has no field %s", cstr(c, head), cstr(c, v->name), cstr(c, f->name)); ft = ty_prim(TY_ERR); }
    } else if (i < v->fields.len) {
      ft = variant_field_type(c, t, v, v->fields.items[i].name);
    } else {
      ERR(c, "E-TYPE-06", p->line, "%s.%s has %d field%s", cstr(c, head), cstr(c, v->name), v->fields.len, v->fields.len == 1 ? "" : "s");
      ft = ty_prim(TY_ERR);
    }
    check_pattern(c, f->pat, ft);
  }
}

typedef VEC(Pattern *) PatVec;

static bool pats_cover(C *c, Ty *t, PatVec *pats, Buf *missing);

// Collects sub-patterns of arms whose head is `variant_name` (NULL for Some/Ok/Err by head).
static bool cover_case(C *c, Ty *inner, PatVec *pats, const char *head, int field_index, Buf *missing, const char *label) {
  PatVec sub = {0};
  bool any = false;
  for (int i = 0; i < pats->len; i++) {
    Pattern *p = pats->items[i];
    if (p->kind != PAT_VARIANT) continue;
    Str h = p->path.items[p->path.len - 1];
    if (!str_eq_c(h, head)) continue;
    any = true;
    if (field_index < 0) return true;  // unit variant
    if (p->fields.len == 0) return true;  // fields omitted: matches everything
    if (p->fields.len == 1 && field_index == 0) { VEC_PUSH(c->a, &sub, p->fields.items[0].pat); }
    else {
      bool all = true;
      for (int j = 0; j < p->fields.len; j++) if (!pat_irrefutable(p->fields.items[j].pat)) all = false;
      if (all) return true;
    }
  }
  if (!any) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, label); return false; }
  if (sub.len == 0) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, label); return false; }
  Buf inner_missing = {0};
  bool ok = pats_cover(c, inner, &sub, &inner_missing);
  if (!ok) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, label); }
  buf_free(&inner_missing);
  return ok;
}

static bool pats_cover(C *c, Ty *t, PatVec *pats, Buf *missing) {
  for (int i = 0; i < pats->len; i++) if (pat_irrefutable(pats->items[i])) return true;
  if (ty_is_err(t)) return true;
  switch (t->kind) {
    case TY_OPTION: {
      bool a = cover_case(c, NULL, pats, "None", -1, missing, "None");
      bool b = cover_case(c, t->a, pats, "Some", 0, missing, "Some(_)");
      return a && b;
    }
    case TY_RESULT: {
      bool a = cover_case(c, t->a, pats, "Ok", 0, missing, "Ok(_)");
      bool b = cover_case(c, t->b, pats, "Err", 0, missing, "Err(_)");
      return a && b;
    }
    case TY_BOOL: {
      bool t_seen = false, f_seen = false;
      for (int i = 0; i < pats->len; i++) {
        Pattern *p = pats->items[i];
        if (p->kind == PAT_LIT && p->lit->kind == E_BOOL) { if (p->lit->bool_val) t_seen = true; else f_seen = true; }
      }
      if (!t_seen) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, "true"); }
      if (!f_seen) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, "false"); }
      return t_seen && f_seen;
    }
    case TY_ENUM: {
      if (!t->decl) { if (missing->len) buf_puts(missing, ", "); buf_puts(missing, "_"); return false; }
      bool all = true;
      for (int i = 0; i < t->decl->variants.len; i++) {
        Variant *v = &t->decl->variants.items[i];
        Buf label = {0};
        buf_str(&label, t->name); buf_puts(&label, "."); buf_str(&label, v->name);
        if (v->fields.len) buf_puts(&label, "(..)");
        Ty *inner = v->fields.len == 1 ? variant_field_type(c, t, v, v->fields.items[0].name) : NULL;
        bool ok = cover_case(c, inner, pats, cstr(c, v->name), v->fields.len == 0 ? -1 : (v->fields.len == 1 ? 0 : 1), missing, label.p);
        buf_free(&label);
        if (!ok) all = false;
      }
      return all;
    }
    default:
      if (missing->len) buf_puts(missing, ", ");
      buf_puts(missing, "_");
      return false;
  }
}

// ---- expressions -------------------------------------------------------

static Ty *check_expr(C *c, Expr *e, Ty *expected) {
  Ty *t = infer(c, e, expected);
  if (expected && !ty_eq(t, expected))
    ERR(c, "E-TYPE-01", e->line, "expected %s, got %s", tstr(c, expected), tstr(c, t));
  return t;
}

static Ty *infer_call(C *c, Expr *e, Ty *expected);

static Ty *infer_binary(C *c, Expr *e) {
  Arena *a = c->a;
  TokKind op = e->op;
  if (op == T_DOTDOT) {
    check_expr(c, e->a, ty_prim(TY_INT));
    check_expr(c, e->b, ty_prim(TY_INT));
    return ty_list(a, ty_prim(TY_INT));
  }
  if (op == T_ANDAND || op == T_OROR) {
    check_expr(c, e->a, ty_prim(TY_BOOL));
    check_expr(c, e->b, ty_prim(TY_BOOL));
    return ty_prim(TY_BOOL);
  }
  Ty *l = infer(c, e->a, NULL);
  Ty *r = infer(c, e->b, is_contextual(e->b) ? l : NULL);
  if (ty_is_err(l) || ty_is_err(r)) return (op == T_EQ || op == T_NE || op == T_LT || op == T_LE || op == T_GT || op == T_GE) ? ty_prim(TY_BOOL) : ty_prim(TY_ERR);
  if (op == T_EQ || op == T_NE) {
    if (!ty_eq(l, r)) ERR(c, "E-TYPE-01", e->line, "cannot compare %s with %s", tstr(c, l), tstr(c, r));
    return ty_prim(TY_BOOL);
  }
  if (op == T_LT || op == T_LE || op == T_GT || op == T_GE) {
    bool ordered = l->kind == TY_INT || l->kind == TY_FLOAT || l->kind == TY_STRING;
    if (!ordered || !ty_eq(l, r)) ERR(c, "E-TYPE-02", e->line, "operator %s needs same-type Int, Float or String operands: got %s and %s", tok_name(op), tstr(c, l), tstr(c, r));
    return ty_prim(TY_BOOL);
  }
  // arithmetic
  if (l->kind == TY_STRING || r->kind == TY_STRING) {
    ERR(c, "E-TYPE-02", e->line, "operator `%s` is not defined on String", tok_name(op));
    return ty_prim(TY_STRING);
  }
  if (!((l->kind == TY_INT && r->kind == TY_INT) || (l->kind == TY_FLOAT && r->kind == TY_FLOAT))) {
    ERR(c, "E-TYPE-02", e->line, "operator `%s` needs same-type Int or Float operands: got %s and %s", tok_name(op), tstr(c, l), tstr(c, r));
    return ty_prim(TY_ERR);
  }
  return l;
}

static Ty *infer_match(C *c, Expr *e, Ty *expected) {
  Ty *subject = infer(c, e->a, NULL);
  Ty *result = expected;
  PatVec unguarded = {0};
  bool irrefutable_seen = false;
  for (int i = 0; i < e->arms.len; i++) {
    MatchArm *arm = &e->arms.items[i];
    if (irrefutable_seen) ERR(c, "E-MATCH-02", arm->line, "unreachable arm");
    push_scope(c);
    check_pattern(c, arm->pat, subject);
    if (arm->guard) check_expr(c, arm->guard, ty_prim(TY_BOOL));
    else VEC_PUSH(c->a, &unguarded, arm->pat);
    Ty *bt = infer(c, arm->body, result && result->kind != TY_NEVER ? result : NULL);
    if (!result || result->kind == TY_NEVER) result = bt;
    else if (!ty_eq(bt, result)) ERR(c, "E-TYPE-04", arm->body->line, "branches have different types: %s and %s", tstr(c, result), tstr(c, bt));
    if (!arm->guard && pat_irrefutable(arm->pat)) irrefutable_seen = true;
    pop_scope(c);
  }
  Buf missing = {0};
  if (!pats_cover(c, subject, &unguarded, &missing))
    ERR(c, "E-MATCH-01", e->line, "non-exhaustive match; missing: %s", missing.len ? missing.p : "_");
  buf_free(&missing);
  return result ? result : ty_prim(TY_UNIT);
}

static Ty *infer_if(C *c, Expr *e, Ty *expected) {
  check_expr(c, e->a, ty_prim(TY_BOOL));
  if (!e->else_block && !e->else_if) {
    push_scope(c);
    check_block(c, e->block, NULL);
    pop_scope(c);
    if (expected && expected->kind != TY_UNIT && !ty_is_err(expected))
      ERR(c, "E-TYPE-04", e->line, "`if` without `else` has type Unit, expected %s", tstr(c, expected));
    return ty_prim(TY_UNIT);
  }
  push_scope(c);
  Ty *t1 = check_block(c, e->block, expected);
  pop_scope(c);
  // a branch that only panics or returns (Never) takes the type of the other branch
  Ty *want = expected ? expected : (t1->kind == TY_NEVER ? NULL : t1);
  Ty *t2;
  if (e->else_if) t2 = infer_if(c, e->else_if, want);
  else { push_scope(c); t2 = check_block(c, e->else_block, want); pop_scope(c); }
  if (!ty_eq(t1, t2) || (expected && (!ty_eq(t1, expected) || !ty_eq(t2, expected))))
    ERR(c, "E-TYPE-04", e->line, "branches have different types: %s and %s", tstr(c, expected ? expected : t1), tstr(c, ty_eq(t1, want) ? t2 : t1));
  if (expected) return expected;
  return t1->kind == TY_NEVER ? t2 : t1;
}

// Field type as declared, with the struct's generic parameters left as TY_PARAM.
static Ty *raw_field_type(C *c, Decl *d, Str field) {
  for (int i = 0; i < d->params.len; i++)
    if (str_eq(d->params.items[i].name, field)) {
      StrVec *saved = c->generics;
      c->generics = &d->generics;
      Ty *ft = resolve_type(c, d->params.items[i].type);
      c->generics = saved;
      return ft;
    }
  return NULL;
}

static Ty *infer_struct_lit(C *c, Expr *e) {
  Decl *d = find_decl(&c->structs, e->type_name);
  if (!d) { ERR(c, "E-NAME-01", e->line, "unknown struct %s", cstr(c, e->type_name)); return ty_prim(TY_ERR); }
  Ty *st = ty_named(c->a, TY_STRUCT, e->type_name, d);
  bool *seen = arena_alloc(c->a, sizeof(bool) * (size_t)(d->params.len + 1));
  bool fields_checked = false;
  if (e->base) {
    if (d->generics.len) {
      Ty *bt = infer(c, e->base, NULL);
      if (bt->kind == TY_STRUCT && bt->decl == d) st = bt;
      else if (!ty_is_err(bt)) ERR(c, "E-TYPE-01", e->line, "expected %s, got %s", cstr(c, e->type_name), tstr(c, bt));
    } else {
      check_expr(c, e->base, st);
    }
  } else if (d->generics.len) {
    // infer the type arguments from the field values, in source order
    TyVec binds = {0};
    for (int i = 0; i < e->fields.len; i++) {
      FieldInit *f = &e->fields.items[i];
      Ty *raw = raw_field_type(c, d, f->name);
      if (!raw) { ERR(c, "E-TYPE-06", e->line, "%s has no field %s", cstr(c, e->type_name), cstr(c, f->name)); infer(c, f->value, NULL); continue; }
      for (int j = 0; j < d->params.len; j++) if (str_eq(d->params.items[j].name, f->name)) seen[j] = true;
      Ty *known = ty_subst(c->a, raw, &d->generics, &binds);
      Ty *vt = infer(c, f->value, ty_has_param(known) ? NULL : known);
      if (!ty_unify(c->a, raw, vt, &d->generics, &binds))
        ERR(c, "E-TYPE-01", f->value->line, "expected %s, got %s", tstr(c, known), tstr(c, vt));
    }
    for (int i = 0; i < d->generics.len; i++) {
      Ty *b = i < binds.len ? binds.items[i] : NULL;
      if (!b) { ERR(c, "E-TYPE-05", e->line, "cannot infer %s for %s", cstr(c, d->generics.items[i]), cstr(c, e->type_name)); b = ty_prim(TY_ERR); }
      VEC_PUSH(c->a, &st->args, b);
    }
    fields_checked = true;
  }
  if (!fields_checked) {
    for (int i = 0; i < e->fields.len; i++) {
      FieldInit *f = &e->fields.items[i];
      Ty *ft = struct_field(c, st, f->name);
      if (!ft) { ERR(c, "E-TYPE-06", e->line, "%s has no field %s", cstr(c, e->type_name), cstr(c, f->name)); infer(c, f->value, NULL); continue; }
      for (int j = 0; j < d->params.len; j++) if (str_eq(d->params.items[j].name, f->name)) seen[j] = true;
      check_expr(c, f->value, ft);
    }
  }
  if (!e->base) {
    for (int j = 0; j < d->params.len; j++)
      if (!seen[j]) ERR(c, "E-TYPE-06", e->line, "missing field %s in %s", cstr(c, d->params.items[j].name), cstr(c, e->type_name));
  }
  return st;
}

static Ty *infer_fn_expr(C *c, Expr *e) {
  if (!e->ret) ERR(c, "E-TYPE-03", e->line, "missing return type on anonymous fn");
  Ty *ret = e->ret ? resolve_type(c, e->ret) : ty_prim(TY_ERR);
  TyVec ps = {0};
  for (int i = 0; i < e->params.len; i++) { Ty *t = resolve_type(c, e->params.items[i].type); VEC_PUSH(c->a, &ps, t); }
  static int next_fn_id = 1;
  e->id = next_fn_id++;
  FnCtx fx = {.ret = ret, .name = STR_LIT("anonymous fn"), .depth = c->fn ? c->fn->depth + 1 : 1, .outer = c->fn, .fn_expr = e};
  c->fn = &fx;
  int saved_loops = c->loop_depth;
  c->loop_depth = 0;
  push_scope(c);
  for (int i = 0; i < e->params.len; i++) {
    e->params.items[i].ty = ps.items[i];
    define(c, e->params.items[i].name, ps.items[i], false, e->params.items[i].line, true);
  }
  Ty *bt = check_block(c, e->block, ret->kind == TY_UNIT ? NULL : ret);
  if (ret->kind != TY_UNIT && !ty_eq(bt, ret)) ERR(c, "E-TYPE-01", e->line, "anonymous fn returns %s, body has type %s", tstr(c, ret), tstr(c, bt));
  pop_scope(c);
  c->loop_depth = saved_loops;
  c->fn = fx.outer;
  return ty_fn(c->a, ps, ret);
}

static Ty *infer_inner(C *c, Expr *e, Ty *expected);

static Ty *infer(C *c, Expr *e, Ty *expected) {
  Ty *t = infer_inner(c, e, expected);
  e->ty = t;
  return t;
}

static Ty *infer_inner(C *c, Expr *e, Ty *expected) {
  Arena *a = c->a;
  switch (e->kind) {
    case E_INT: return ty_prim(TY_INT);
    case E_FLOAT: return ty_prim(TY_FLOAT);
    case E_BOOL: return ty_prim(TY_BOOL);
    case E_STRING:
      for (int i = 0; i < e->args.len; i++) infer(c, e->args.items[i], NULL);
      return ty_prim(TY_STRING);
    case E_IDENT: {
      if (str_eq_c(e->text, "None")) {
        if (expected && expected->kind == TY_OPTION) return expected;
        if (expected && ty_is_err(expected)) return ty_prim(TY_ERR);
        ERR(c, "E-TYPE-05", e->line, "cannot infer the type of None");
        return ty_prim(TY_ERR);
      }
      Sym *y = use_sym(c, e->text, e->line);
      if (y) { e->call_kind = CK_VALUE; return y->type; }
      Decl *f = find_decl(&c->fns, e->text);
      if (f) {
        e->call_kind = CK_FN;
        if (f->generics.len) { ERR(c, "E-TYPE-05", e->line, "generic fn %s must be called; it cannot be passed as a value", cstr(c, e->text)); return ty_prim(TY_ERR); }
        return fn_type(c, f, NULL);
      }
      Decl *k = find_decl(&c->consts, e->text);
      if (k) { e->call_kind = CK_STATIC; return resolve_type(c, k->ret); }
      if (is_type_name(c, e->text)) { ERR(c, "E-NAME-01", e->line, "%s is a type, not a value", cstr(c, e->text)); return ty_prim(TY_ERR); }
      ERR(c, "E-NAME-01", e->line, "unknown name %s", cstr(c, e->text));
      return ty_prim(TY_ERR);
    }
    case E_FIELD: {
      Expr *obj = e->a;
      if (obj->kind == E_IDENT && !find_sym(c, obj->text)) {
        Decl *en = find_decl(&c->enums, obj->text);
        if (en) {
          Variant *v = enum_variant(en, e->text);
          if (!v) { ERR(c, "E-NAME-01", e->line, "%s has no variant %s", cstr(c, obj->text), cstr(c, e->text)); return ty_prim(TY_ERR); }
          if (v->fields.len) ERR(c, "E-TYPE-06", e->line, "%s.%s needs (%s: ...)", cstr(c, obj->text), cstr(c, v->name), cstr(c, v->fields.items[0].name));
          if (en->generics.len) {
            if (expected && expected->kind == TY_ENUM && expected->decl == en) return expected;
            if (expected && ty_is_err(expected)) return ty_prim(TY_ERR);
            ERR(c, "E-TYPE-05", e->line, "cannot infer the type arguments of %s.%s", cstr(c, obj->text), cstr(c, e->text));
            return ty_prim(TY_ERR);
          }
          return ty_named(a, TY_ENUM, obj->text, en);
        }
        if (is_type_name(c, obj->text)) { ERR(c, "E-NAME-01", e->line, "%s.%s is not a value; call it", cstr(c, obj->text), cstr(c, e->text)); return ty_prim(TY_ERR); }
        for (int i = 0; i < c->modules.len; i++)
          if (str_eq(c->modules.items[i], obj->text)) { ERR(c, "E-NAME-01", e->line, "module %s has no %s in Stage 0", cstr(c, obj->text), cstr(c, e->text)); return ty_prim(TY_ERR); }
      }
      Ty *t = infer(c, obj, NULL);
      if (ty_is_err(t)) return t;
      if (t->kind == TY_STRUCT) {
        Ty *ft = struct_field(c, t, e->text);
        if (ft) return ft;
        ERR(c, "E-NAME-01", e->line, "%s has no field %s", tstr(c, t), cstr(c, e->text));
        return ty_prim(TY_ERR);
      }
      if (t->kind == TY_ENTRY) {
        if (str_eq_c(e->text, "key")) return t->a;
        if (str_eq_c(e->text, "value")) return t->b;
      }
      ERR(c, "E-NAME-01", e->line, "%s has no field %s", tstr(c, t), cstr(c, e->text));
      return ty_prim(TY_ERR);
    }
    case E_INDEX: {
      Ty *t = infer(c, e->a, NULL);
      check_expr(c, e->b, ty_prim(TY_INT));
      if (ty_is_err(t)) return t;
      if (t->kind == TY_LIST) return t->a;
      ERR(c, "E-TYPE-01", e->line, "cannot index %s; only List supports [i]", tstr(c, t));
      return ty_prim(TY_ERR);
    }
    case E_CALL: return infer_call(c, e, expected);
    case E_UNARY: {
      if (e->op == T_BANG) { check_expr(c, e->a, ty_prim(TY_BOOL)); return ty_prim(TY_BOOL); }
      Ty *t = infer(c, e->a, NULL);
      if (ty_is_err(t)) return t;
      if (t->kind != TY_INT && t->kind != TY_FLOAT) { ERR(c, "E-TYPE-02", e->line, "unary `-` needs Int or Float, got %s", tstr(c, t)); return ty_prim(TY_ERR); }
      return t;
    }
    case E_BINARY: return infer_binary(c, e);
    case E_TRY: {
      Ty *t = infer(c, e->a, NULL);
      if (ty_is_err(t)) return t;
      Ty *ret = c->fn ? c->fn->ret : NULL;
      const char *fname = c->fn ? cstr(c, c->fn->name) : "test";
      if (t->kind == TY_RESULT) {
        if (!ret || ret->kind != TY_RESULT || !ty_eq(ret->b, t->b)) {
          if (ret && ty_is_err(ret)) return t->a;
          ERR(c, "E-RES-02", e->line, "`?` in fn %s, which returns %s, not Result<_, %s>", fname, tstr(c, ret ? ret : ty_prim(TY_UNIT)), tstr(c, t->b));
        }
        return t->a;
      }
      if (t->kind == TY_OPTION) {
        if (!ret || ret->kind != TY_OPTION) {
          if (ret && ty_is_err(ret)) return t->a;
          ERR(c, "E-RES-02", e->line, "`?` on Option in fn %s, which returns %s, not Option", fname, tstr(c, ret ? ret : ty_prim(TY_UNIT)));
        }
        return t->a;
      }
      ERR(c, "E-TYPE-01", e->line, "`?` needs Result or Option, got %s", tstr(c, t));
      return ty_prim(TY_ERR);
    }
    case E_IF: return infer_if(c, e, expected);
    case E_MATCH: return infer_match(c, e, expected);
    case E_STRUCT: return infer_struct_lit(c, e);
    case E_LIST: {
      if (e->args.len == 0) {
        if (expected && expected->kind == TY_LIST) return expected;
        if (expected && ty_is_err(expected)) return ty_prim(TY_ERR);
        ERR(c, "E-TYPE-05", e->line, "cannot infer the element type of []");
        return ty_prim(TY_ERR);
      }
      Ty *elem = infer(c, e->args.items[0], expected && expected->kind == TY_LIST ? expected->a : NULL);
      for (int i = 1; i < e->args.len; i++) check_expr(c, e->args.items[i], elem);
      return ty_list(a, elem);
    }
    case E_FN: return infer_fn_expr(c, e);
    case E_BLOCK: { push_scope(c); Ty *t = check_block(c, e->block, expected); pop_scope(c); return t; }
    case E_TYPECTOR: {
      Ty *t = resolve_type(c, e->type);
      if (!ty_is_err(t) && t->kind != TY_LIST && t->kind != TY_MAP) ERR(c, "E-TYPE-06", e->line, "only List<T>() and Map<K, V>() can be built this way");
      return t;
    }
    case E_PAREN: return ty_prim(TY_ERR);
  }
  return ty_prim(TY_ERR);
}

static Ty *infer_call(C *c, Expr *e, Ty *expected) {
  Arena *a = c->a;
  Expr *f = e->a;
  // 1. plain name: prelude, user function, or a local function value
  if (f->kind == E_IDENT && !find_sym(c, f->text)) {
    Str n = f->text;
    e->call_kind = CK_PRELUDE;
    if (str_eq_c(n, "Some")) {
      check_arg_count(c, e, 1, "Some");
      Ty *x = arg_or_err(c, e, 0, expected && expected->kind == TY_OPTION ? expected->a : NULL);
      return ty_option(a, x);
    }
    if (str_eq_c(n, "Ok") || str_eq_c(n, "Err")) {
      bool ok = str_eq_c(n, "Ok");
      check_arg_count(c, e, 1, cstr(c, n));
      if (expected && expected->kind == TY_RESULT) { arg_or_err(c, e, 0, ok ? expected->a : expected->b); return expected; }
      Ty *x = arg_or_err(c, e, 0, NULL);
      if (expected && ty_is_err(expected)) return ty_prim(TY_ERR);
      ERR(c, "E-TYPE-05", e->line, "cannot infer the %s type of %s(...)", ok ? "error" : "ok", cstr(c, n));
      (void)x;
      return ty_prim(TY_ERR);
    }
    if (str_eq_c(n, "assert")) { check_arg_count(c, e, 1, "assert"); arg_or_err(c, e, 0, ty_prim(TY_BOOL)); return ty_prim(TY_UNIT); }
    if (str_eq_c(n, "assert_eq")) {
      check_arg_count(c, e, 2, "assert_eq");
      if (e->args.len == 2) {
        int first = is_contextual(e->args.items[0]) && !is_contextual(e->args.items[1]) ? 1 : 0;
        Ty *t = infer(c, e->args.items[first], NULL);
        check_expr(c, e->args.items[1 - first], t);
      }
      return ty_prim(TY_UNIT);
    }
    if (str_eq_c(n, "panic")) { check_arg_count(c, e, 1, "panic"); arg_or_err(c, e, 0, ty_prim(TY_STRING)); return ty_prim(TY_NEVER); }
    Decl *d = find_decl(&c->fns, n);
    if (d) {
      e->call_kind = CK_FN;
      Ty *ft = fn_type(c, d, NULL);
      return call_fn_type(c, e, ft, d->generics.len ? &d->generics : NULL, cstr(c, n), expected);
    }
    e->call_kind = CK_UNKNOWN;
    if (find_decl(&c->structs, n)) { ERR(c, "E-SYN-01", e->line, "structs are built with %s { field: value }", cstr(c, n)); return ty_prim(TY_ERR); }
    ERR(c, "E-NAME-01", e->line, "unknown function %s", cstr(c, n));
    for (int i = 0; i < e->args.len; i++) infer(c, e->args.items[i], NULL);
    return ty_prim(TY_ERR);
  }
  // 2. Type.name(...): static functions and enum variant constructors
  if (f->kind == E_FIELD && f->a->kind == E_IDENT && !find_sym(c, f->a->text)) {
    Str tn = f->a->text, fnm = f->text;
    e->call_kind = CK_STATIC;
    if (str_eq_c(tn, "Int") && str_eq_c(fnm, "parse")) { check_arg_count(c, e, 1, "Int.parse"); arg_or_err(c, e, 0, ty_prim(TY_STRING)); return ty_result(a, ty_prim(TY_INT), ty_named(a, TY_ENUM, STR_LIT("ParseError"), NULL)); }
    if (str_eq_c(tn, "Float") && str_eq_c(fnm, "from")) { check_arg_count(c, e, 1, "Float.from"); arg_or_err(c, e, 0, ty_prim(TY_INT)); return ty_prim(TY_FLOAT); }
    if (str_eq_c(tn, "String") && str_eq_c(fnm, "from")) {
      check_arg_count(c, e, 1, "String.from");
      Ty *t = arg_or_err(c, e, 0, NULL);
      if (!ty_is_err(t) && t->kind != TY_INT && t->kind != TY_FLOAT && t->kind != TY_BOOL) ERR(c, "E-TYPE-01", e->line, "String.from takes Int, Float or Bool, got %s", tstr(c, t));
      return ty_prim(TY_STRING);
    }
    Decl *en = find_decl(&c->enums, tn);
    if (en) {
      e->call_kind = CK_VARIANT;
      Variant *v = enum_variant(en, fnm);
      if (!v) { ERR(c, "E-NAME-01", e->line, "%s has no variant %s", cstr(c, tn), cstr(c, fnm)); return ty_prim(TY_ERR); }
      Ty *et = ty_named(a, TY_ENUM, tn, en);
      if (en->generics.len && expected && expected->kind == TY_ENUM && expected->decl == en) et = expected;
      if (v->fields.len == 0) { ERR(c, "E-TYPE-06", e->line, "%s.%s takes no fields; write it without ()", cstr(c, tn), cstr(c, fnm)); return et; }
      if (e->args.len != v->fields.len) ERR(c, "E-TYPE-06", e->line, "%s.%s takes %d field%s, got %d", cstr(c, tn), cstr(c, fnm), v->fields.len, v->fields.len == 1 ? "" : "s", e->args.len);
      bool generic_unbound = en->generics.len > 0 && et->args.len == 0;
      TyVec binds = {0};
      for (int i = 0; i < e->args.len; i++) {
        Str an = e->arg_names.len > i ? e->arg_names.items[i] : (Str){0};
        Ty *raw = NULL;
        if (an.len) {
          for (int k = 0; k < v->fields.len; k++) if (str_eq(v->fields.items[k].name, an)) raw = v->fields.items[k].ty;
          if (!raw) { ERR(c, "E-TYPE-06", e->line, "%s.%s has no field %s", cstr(c, tn), cstr(c, fnm), cstr(c, an)); infer(c, e->args.items[i], NULL); continue; }
        } else if (i < v->fields.len) {
          ERR(c, "E-TYPE-06", e->line, "%s.%s fields are named: %s: ...", cstr(c, tn), cstr(c, fnm), cstr(c, v->fields.items[i].name));
          raw = v->fields.items[i].ty;
        } else { infer(c, e->args.items[i], NULL); continue; }
        if (!raw) raw = ty_prim(TY_ERR);
        if (generic_unbound) {
          Ty *known = ty_subst(a, raw, &en->generics, &binds);
          Ty *vt = infer(c, e->args.items[i], ty_has_param(known) ? NULL : known);
          if (!ty_unify(a, raw, vt, &en->generics, &binds)) ERR(c, "E-TYPE-01", e->args.items[i]->line, "expected %s, got %s", tstr(c, known), tstr(c, vt));
        } else {
          check_expr(c, e->args.items[i], ty_subst(a, raw, &en->generics, &et->args));
        }
      }
      if (generic_unbound) {
        for (int i = 0; i < en->generics.len; i++) {
          Ty *b = i < binds.len ? binds.items[i] : NULL;
          if (!b) { ERR(c, "E-TYPE-05", e->line, "cannot infer %s for %s.%s", cstr(c, en->generics.items[i]), cstr(c, tn), cstr(c, fnm)); b = ty_prim(TY_ERR); }
          VEC_PUSH(a, &et->args, b);
        }
      }
      return et;
    }
    if (is_type_name(c, tn)) { ERR(c, "E-NAME-01", e->line, "no function %s on type %s", cstr(c, fnm), cstr(c, tn)); return ty_prim(TY_ERR); }
    bool is_module = false;
    for (int i = 0; i < c->modules.len; i++) if (str_eq(c->modules.items[i], tn)) is_module = true;
    if (is_module) {
      e->call_kind = CK_MODULE;
      if (str_eq_c(tn, "json")) {
        Decl *jd = find_decl(&c->enums, STR_LIT("JsonError"));
        Ty *jerr = ty_named(a, TY_ENUM, STR_LIT("JsonError"), jd);
        if (str_eq_c(fnm, "encode")) {
          check_arg_count(c, e, 1, "json.encode");
          Ty *vt = arg_or_err(c, e, 0, NULL);
          if (!json_able(c, vt)) ERR(c, "E-JSON-01", e->line, "cannot convert %s to JSON", tstr(c, vt));
          return ty_prim(TY_STRING);
        }
        if (str_eq_c(fnm, "decode")) {
          if (f->explicit_types.len != 1) { ERR(c, "E-TYPE-06", e->line, "json.decode needs one type argument: json.decode<T>(s)"); for (int j = 0; j < e->args.len; j++) infer(c, e->args.items[j], NULL); return ty_prim(TY_ERR); }
          Ty *T = resolve_type(c, f->explicit_types.items[0]);
          if (!json_able(c, T)) ERR(c, "E-JSON-01", e->line, "cannot convert JSON to %s", tstr(c, T));
          check_arg_count(c, e, 1, "json.decode");
          arg_or_err(c, e, 0, ty_prim(TY_STRING));
          VEC_PUSH(a, &e->type_args, T);
          return ty_result(a, T, jerr);
        }
      }
      if (str_eq_c(tn, "http")) {
        Decl *rq = find_decl(&c->structs, STR_LIT("Request")), *rs = find_decl(&c->structs, STR_LIT("Response"));
        Ty *req = ty_named(a, TY_STRUCT, STR_LIT("Request"), rq), *res = ty_named(a, TY_STRUCT, STR_LIT("Response"), rs);
        if (str_eq_c(fnm, "serve")) {
          check_arg_count(c, e, 2, "http.serve");
          arg_or_err(c, e, 0, ty_prim(TY_INT));
          TyVec ps = {0};
          VEC_PUSH(a, &ps, req);
          arg_or_err(c, e, 1, ty_fn(a, ps, res));
          return ty_prim(TY_UNIT);
        }
        if (str_eq_c(fnm, "response") || str_eq_c(fnm, "json")) {
          check_arg_count(c, e, 2, str_eq_c(fnm, "json") ? "http.json" : "http.response");
          arg_or_err(c, e, 0, ty_prim(TY_INT));
          arg_or_err(c, e, 1, ty_prim(TY_STRING));
          return res;
        }
        if (str_eq_c(fnm, "request")) {
          check_arg_count(c, e, 3, "http.request");
          for (int j = 0; j < 3; j++) arg_or_err(c, e, j, ty_prim(TY_STRING));
          return req;
        }
      }
      ERR(c, "E-NAME-01", e->line, "module %s has no function %s", cstr(c, tn), cstr(c, fnm));
      for (int j = 0; j < e->args.len; j++) infer(c, e->args.items[j], NULL);
      return ty_prim(TY_ERR);
    }
    // falls through to a method call on an unknown name (reported there)
  }
  // 3. value.method(...)
  if (f->kind == E_FIELD) {
    Ty *recv = infer(c, f->a, NULL);
    if (ty_is_err(recv)) { for (int i = 0; i < e->args.len; i++) infer(c, e->args.items[i], NULL); return recv; }
    bool found;
    e->call_kind = CK_BUILTIN;
    Ty *r = builtin_method(c, recv, f->text, e, expected, &found);
    if (found) return r;
    if (recv->kind == TY_STRUCT || recv->kind == TY_ENUM) {
      Decl *m = find_method(c, recv->name, f->text);
      if (m) {
        e->call_kind = CK_METHOD;
        e->method = m;
        Ty *ft = fn_type(c, m, recv);
        if (recv->decl && recv->decl->generics.len) ft = ty_subst(a, ft, &recv->decl->generics, &recv->args);
        if (ft->params.len == 0 || !str_eq_c(m->params.items[0].name, "self")) { ERR(c, "E-TYPE-06", e->line, "%s.%s has no self and cannot be called as a method", tstr(c, recv), cstr(c, f->text)); return ft->ret; }
        TyVec rest = {0};
        for (int i = 1; i < ft->params.len; i++) VEC_PUSH(a, &rest, ft->params.items[i]);
        Ty *callable = ty_fn(a, rest, ft->ret);
        Buf what = {0};
        buf_str(&what, recv->name); buf_puts(&what, "."); buf_str(&what, f->text);
        Ty *res = call_fn_type(c, e, callable, m->generics.len ? &m->generics : NULL, arena_cstr(a, str_from(what.p, what.len)), expected);
        buf_free(&what);
        return res;
      }
    }
    ERR(c, "E-NAME-01", e->line, "no method %s on %s", cstr(c, f->text), tstr(c, recv));
    for (int i = 0; i < e->args.len; i++) infer(c, e->args.items[i], NULL);
    return ty_prim(TY_ERR);
  }
  // 4. any other callee: must have a function type
  e->call_kind = CK_VALUE;
  Ty *ft = infer(c, f, NULL);
  if (ty_is_err(ft)) { for (int i = 0; i < e->args.len; i++) infer(c, e->args.items[i], NULL); return ft; }
  if (ft->kind != TY_FN) { ERR(c, "E-TYPE-01", e->line, "%s is not a function", tstr(c, ft)); return ty_prim(TY_ERR); }
  return call_fn_type(c, e, ft, NULL, f->kind == E_IDENT ? cstr(c, f->text) : "function value", expected);
}

// ---- statements and blocks ---------------------------------------------

static void report_unused_result(C *c, Expr *e, Ty *t) {
  if (t && t->kind == TY_RESULT && e->kind != E_TRY)
    ERR(c, "E-RES-01", e->line, "unused Result from %s()", callee_name(c, e));
}

static void check_stmt(C *c, Stmt *s) {
  switch (s->kind) {
    case S_LET: {
      Ty *t = infer(c, s->value, NULL);
      s->ty = t;
      define(c, s->name, t, s->mutable, s->line, false);
      break;
    }
    case S_ASSIGN: {
      Expr *tg = s->target;
      if (tg->kind != E_IDENT && tg->kind != E_FIELD && tg->kind != E_INDEX) { ERR(c, "E-SYN-01", s->line, "cannot assign to this expression"); infer(c, s->value, NULL); break; }
      Ty *tt = infer(c, tg, NULL);
      if (!is_mutable_place(c, tg)) ERR(c, "E-BIND-02", s->line, "assignment to let %s", place_name(c, tg));
      check_expr(c, s->value, ty_is_err(tt) ? NULL : tt);
      break;
    }
    case S_EXPR: {
      Ty *t = infer(c, s->value, NULL);
      report_unused_result(c, s->value, t);
      break;
    }
    case S_RETURN: {
      Ty *ret = c->fn ? c->fn->ret : ty_prim(TY_UNIT);
      if (s->value) check_expr(c, s->value, ret);
      else if (ret->kind != TY_UNIT && !ty_is_err(ret)) ERR(c, "E-TYPE-01", s->line, "return needs a value of type %s", tstr(c, ret));
      break;
    }
    case S_BREAK: case S_CONTINUE:
      if (c->loop_depth == 0) ERR(c, "E-SYN-03", s->line, "`%s` outside a loop", s->kind == S_BREAK ? "break" : "continue");
      break;
    case S_FOR: {
      Ty *it = infer(c, s->value, NULL);
      Ty *elem = ty_prim(TY_ERR);
      if (!ty_is_err(it)) {
        if (it->kind == TY_LIST) elem = it->a;
        else if (it->kind == TY_MAP) ERR(c, "E-TYPE-01", s->line, "iterate a Map with entries()");
        else ERR(c, "E-TYPE-01", s->line, "for needs a range or a List, got %s", tstr(c, it));
      }
      s->ty = elem;
      push_scope(c);
      define(c, s->name, elem, false, s->line, false);
      c->loop_depth++;
      check_block(c, s->body, NULL);
      c->loop_depth--;
      pop_scope(c);
      break;
    }
    case S_WHILE: {
      check_expr(c, s->value, ty_prim(TY_BOOL));
      push_scope(c);
      c->loop_depth++;
      check_block(c, s->body, NULL);
      c->loop_depth--;
      pop_scope(c);
      break;
    }
  }
}

// Type of a block: the last expression statement, or Unit. `expected` flows into the last expression.
static Ty *check_block(C *c, Block *b, Ty *expected) {
  Ty *t = ty_prim(TY_UNIT);
  push_scope(c);  // every block is its own scope: a loop body may shadow the loop variable
  for (int i = 0; i < b->stmts.len; i++) {
    Stmt *s = b->stmts.items[i];
    bool last = i == b->stmts.len - 1;
    if (last && s->kind == S_EXPR) {
      t = infer(c, s->value, expected);
      if (!expected || expected->kind == TY_UNIT) report_unused_result(c, s->value, t);
    } else {
      check_stmt(c, s);
      if (last && s->kind == S_RETURN) t = ty_prim(TY_NEVER);
    }
  }
  pop_scope(c);
  return t;
}

// ---- declarations ------------------------------------------------------

static void check_fn_body(C *c, Decl *d, Ty *self_type) {
  StrVec *saved = c->generics;
  c->generics = &d->generics;
  if (!d->ret) ERR(c, "E-TYPE-03", d->line, "missing return type on fn %s", cstr(c, d->name));
  Ty *ret = d->ret ? resolve_type(c, d->ret) : ty_prim(TY_ERR);
  FnCtx fx = {.ret = ret, .name = d->name, .depth = 0, .outer = NULL};
  c->fn = &fx;
  push_scope(c);
  for (int i = 0; i < d->params.len; i++) {
    Param *p = &d->params.items[i];
    Ty *t;
    if (!p->type) {
      if (self_type && i == 0) t = self_type;
      else { ERR(c, "E-TYPE-03", p->line, "missing type on parameter %s", cstr(c, p->name)); t = ty_prim(TY_ERR); }
    } else t = resolve_type(c, p->type);
    p->ty = t;
    define(c, p->name, t, false, p->line, true);
  }
  d->ty = fn_type(c, d, self_type);
  Ty *bt = check_block(c, d->body, ret->kind == TY_UNIT ? NULL : ret);
  if (!ty_is_err(ret) && ret->kind != TY_UNIT) {
    Stmt *lastst = d->body->stmts.len ? d->body->stmts.items[d->body->stmts.len - 1] : NULL;
    bool ends_with_value = lastst && (lastst->kind == S_EXPR || lastst->kind == S_RETURN);
    if (!ends_with_value) ERR(c, "E-TYPE-01", d->body->end_line, "fn %s must end with a value of type %s", cstr(c, d->name), tstr(c, ret));
    else if (!ty_eq(bt, ret)) ERR(c, "E-TYPE-01", lastst->line, "fn %s returns %s, body has type %s", cstr(c, d->name), tstr(c, ret), tstr(c, bt));
  }
  pop_scope(c);
  c->fn = NULL;
  c->generics = saved;
}

static void check_type_decl(C *c, Decl *d) {
  StrVec *saved = c->generics;
  c->generics = &d->generics;
  if (d->kind == D_STRUCT) {
    for (int i = 0; i < d->params.len; i++) {
      for (int j = 0; j < i; j++)
        if (str_eq(d->params.items[j].name, d->params.items[i].name)) ERR(c, "E-NAME-02", d->params.items[i].line, "duplicate field %s", cstr(c, d->params.items[i].name));
      d->params.items[i].ty = resolve_type(c, d->params.items[i].type);
    }
  } else {
    for (int i = 0; i < d->variants.len; i++) {
      Variant *v = &d->variants.items[i];
      for (int j = 0; j < i; j++)
        if (str_eq(d->variants.items[j].name, v->name)) ERR(c, "E-NAME-02", v->line, "duplicate variant %s", cstr(c, v->name));
      for (int k = 0; k < v->fields.len; k++) v->fields.items[k].ty = resolve_type(c, v->fields.items[k].type);
    }
  }
  c->generics = saved;
}

// Parses std declarations and puts them in front of the module's own declarations.
static void inject_std(Arena *a, Module *m) {
  DeclVec injected = {0};
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    if (d->kind != D_IMPORT || d->path.len != 2 || !str_eq_c(d->path.items[0], "std")) continue;
    const char *src = NULL, *file = NULL;
    if (str_eq_c(d->path.items[1], "json")) { src = STD_JSON_SRC; file = "std/json.yu"; }
    if (str_eq_c(d->path.items[1], "http")) { src = STD_HTTP_SRC; file = "std/http.yu"; }
    if (!src) continue;
    LexResult lx = lex(a, file, str_cstr(src));
    Module *sm = parse_module(a, file, lx);
    for (int j = 0; j < sm->decls.len; j++) {
      sm->decls.items[j]->from_std = true;
      sm->decls.items[j]->line = d->line;
      VEC_PUSH(a, &injected, sm->decls.items[j]);
    }
  }
  if (!injected.len) return;
  for (int i = 0; i < m->decls.len; i++) VEC_PUSH(a, &injected, m->decls.items[i]);
  m->decls = injected;
}

// True for types std.json can encode and decode.
static bool json_able(C *c, Ty *t) {
  switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_ERR: return true;
    case TY_LIST: case TY_OPTION: return json_able(c, t->a);
    case TY_MAP: return t->a->kind == TY_STRING && json_able(c, t->b);
    case TY_STRUCT:
      if (!t->decl) return false;
      for (int i = 0; i < t->decl->params.len; i++) if (!json_able(c, struct_field(c, t, t->decl->params.items[i].name))) return false;
      return true;
    case TY_ENUM:
      if (!t->decl) return false;
      for (int i = 0; i < t->decl->variants.len; i++) if (t->decl->variants.items[i].fields.len) return false;
      return true;
    default: return false;
  }
}

void check_module(Arena *a, Module *m) {
  C ctx = {.a = a, .file = m->file, .m = m};
  C *c = &ctx;
  inject_std(a, m);
  // pass 1: collect top-level names
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    DeclVec *bucket = NULL;
    switch (d->kind) {
      case D_FN: bucket = &c->fns; break;
      case D_STRUCT: bucket = &c->structs; break;
      case D_ENUM: bucket = &c->enums; break;
      case D_CONST: bucket = &c->consts; break;
      case D_IMPL: VEC_PUSH(a, &c->impls, d); continue;
      case D_IMPORT: {
        if (d->path.len != 2 || !str_eq_c(d->path.items[0], "std") ||
            !(str_eq_c(d->path.items[1], "string") || str_eq_c(d->path.items[1], "json") || str_eq_c(d->path.items[1], "http")))
          ERR(c, "E-NAME-01", d->line, "unknown module; Stage 0 knows std.string, std.json and std.http");
        else VEC_PUSH(a, &c->modules, d->path.items[1]);
        continue;
      }
      case D_TEST: continue;
    }
    bool dup = find_decl(&c->fns, d->name) || find_decl(&c->structs, d->name) || find_decl(&c->enums, d->name) || find_decl(&c->consts, d->name);
    if (dup) ERR(c, "E-NAME-02", d->line, "duplicate definition of %s", cstr(c, d->name));
    else VEC_PUSH(a, bucket, d);
  }
  // pass 2: check declarations
  push_scope(c);
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    switch (d->kind) {
      case D_STRUCT: case D_ENUM: check_type_decl(c, d); break;
      case D_FN: check_fn_body(c, d, NULL); break;
      case D_CONST: {
        Ty *t = resolve_type(c, d->ret);
        d->ty = t;
        if (d->value->kind != E_INT && d->value->kind != E_FLOAT && d->value->kind != E_BOOL && d->value->kind != E_STRING)
          ERR(c, "E-SYN-01", d->line, "const %s needs a literal value", cstr(c, d->name));
        check_expr(c, d->value, t);
        break;
      }
      case D_IMPL: {
        Decl *sd = find_decl(&c->structs, d->name);
        Decl *ed = sd ? NULL : find_decl(&c->enums, d->name);
        if (!sd && !ed) { ERR(c, "E-NAME-01", d->line, "impl for unknown type %s", cstr(c, d->name)); break; }
        Decl *td = sd ? sd : ed;
        Ty *self_type = ty_named(a, sd ? TY_STRUCT : TY_ENUM, d->name, td);
        for (int k = 0; k < td->generics.len; k++) { Ty *pt = ty_param(a, td->generics.items[k]); VEC_PUSH(a, &self_type->args, pt); }
        c->outer_generics = td->generics.len ? &td->generics : NULL;
        for (int j = 0; j < d->methods.len; j++) {
          for (int k = 0; k < j; k++)
            if (str_eq(d->methods.items[k]->name, d->methods.items[j]->name)) ERR(c, "E-NAME-02", d->methods.items[j]->line, "duplicate method %s", cstr(c, d->methods.items[j]->name));
          check_fn_body(c, d->methods.items[j], self_type);
        }
        c->outer_generics = NULL;
        break;
      }
      case D_TEST: {
        FnCtx fx = {.ret = ty_prim(TY_UNIT), .name = d->name, .depth = 0, .outer = NULL};
        c->fn = &fx;
        push_scope(c);
        check_block(c, d->body, NULL);
        pop_scope(c);
        c->fn = NULL;
        break;
      }
      case D_IMPORT: break;
    }
  }
  c->scope = NULL;
}
