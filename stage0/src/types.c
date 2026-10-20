#include "types.h"

static Ty prims[TY_ERR + 1];

Ty *ty_prim(TyKind k) {
  prims[k].kind = k;
  return &prims[k];
}

static Ty *mk(Arena *a, TyKind k) {
  Ty *t = NEW(a, Ty);
  t->kind = k;
  return t;
}

Ty *ty_list(Arena *a, Ty *e) { Ty *t = mk(a, TY_LIST); t->a = e; return t; }
Ty *ty_map(Arena *a, Ty *k, Ty *v) { Ty *t = mk(a, TY_MAP); t->a = k; t->b = v; return t; }
Ty *ty_option(Arena *a, Ty *v) { Ty *t = mk(a, TY_OPTION); t->a = v; return t; }
Ty *ty_result(Arena *a, Ty *ok, Ty *err) { Ty *t = mk(a, TY_RESULT); t->a = ok; t->b = err; return t; }
Ty *ty_entry(Arena *a, Ty *k, Ty *v) { Ty *t = mk(a, TY_ENTRY); t->a = k; t->b = v; return t; }
Ty *ty_store(Arena *a, Ty *v) { Ty *t = mk(a, TY_STORE); t->a = v; return t; }
Ty *ty_fn(Arena *a, TyVec params, Ty *ret) { Ty *t = mk(a, TY_FN); t->params = params; t->ret = ret; return t; }
Ty *ty_param(Arena *a, Str name) { Ty *t = mk(a, TY_PARAM); t->name = name; return t; }
Ty *ty_named(Arena *a, TyKind k, Str name, Decl *decl) { Ty *t = mk(a, k); t->name = name; t->decl = decl; return t; }

bool ty_is_err(Ty *t) { return t == NULL || t->kind == TY_ERR; }

bool ty_eq(Ty *x, Ty *y) {
  if (x == NULL || y == NULL) return true;
  if (x->kind == TY_ERR || y->kind == TY_ERR) return true;
  if (x->kind == TY_NEVER || y->kind == TY_NEVER) return true;
  if (x->kind != y->kind) return false;
  switch (x->kind) {
    case TY_LIST: case TY_OPTION: case TY_STORE: return ty_eq(x->a, y->a);
    case TY_MAP: case TY_RESULT: case TY_ENTRY: return ty_eq(x->a, y->a) && ty_eq(x->b, y->b);
    case TY_STRUCT: case TY_ENUM:
      if (x->decl != y->decl || !str_eq(x->name, y->name)) return false;
      if (x->args.len != y->args.len) return false;
      for (int i = 0; i < x->args.len; i++) if (!ty_eq(x->args.items[i], y->args.items[i])) return false;
      return true;
    case TY_FN:
      if (x->params.len != y->params.len) return false;
      for (int i = 0; i < x->params.len; i++) if (!ty_eq(x->params.items[i], y->params.items[i])) return false;
      return ty_eq(x->ret, y->ret);
    case TY_PARAM: case TY_CAP: return str_eq(x->name, y->name);
    default: return true;
  }
}

void ty_print(Buf *b, Ty *t) {
  if (!t) { buf_puts(b, "?"); return; }
  switch (t->kind) {
    case TY_INT: buf_puts(b, "Int"); break;
    case TY_FLOAT: buf_puts(b, "Float"); break;
    case TY_BOOL: buf_puts(b, "Bool"); break;
    case TY_STRING: buf_puts(b, "String"); break;
    case TY_BYTES: buf_puts(b, "Bytes"); break;
    case TY_UNIT: buf_puts(b, "Unit"); break;
    case TY_NEVER: buf_puts(b, "Never"); break;
    case TY_ERR: buf_puts(b, "?"); break;
    case TY_LIST: buf_puts(b, "List<"); ty_print(b, t->a); buf_puts(b, ">"); break;
    case TY_MAP: buf_puts(b, "Map<"); ty_print(b, t->a); buf_puts(b, ", "); ty_print(b, t->b); buf_puts(b, ">"); break;
    case TY_OPTION: buf_puts(b, "Option<"); ty_print(b, t->a); buf_puts(b, ">"); break;
    case TY_RESULT: buf_puts(b, "Result<"); ty_print(b, t->a); buf_puts(b, ", "); ty_print(b, t->b); buf_puts(b, ">"); break;
    case TY_ENTRY: buf_puts(b, "Entry<"); ty_print(b, t->a); buf_puts(b, ", "); ty_print(b, t->b); buf_puts(b, ">"); break;
    case TY_STORE: buf_puts(b, "Store<"); ty_print(b, t->a); buf_puts(b, ">"); break;
    case TY_STRUCT: case TY_ENUM:
      buf_str(b, t->name);
      if (t->args.len) {
        buf_puts(b, "<");
        for (int i = 0; i < t->args.len; i++) { if (i) buf_puts(b, ", "); ty_print(b, t->args.items[i]); }
        buf_puts(b, ">");
      }
      break;
    case TY_FN:
      buf_puts(b, "fn(");
      for (int i = 0; i < t->params.len; i++) { if (i) buf_puts(b, ", "); ty_print(b, t->params.items[i]); }
      buf_puts(b, ") -> ");
      ty_print(b, t->ret);
      break;
    case TY_PARAM: case TY_CAP: buf_str(b, t->name); break;
  }
}

const char *ty_str(Arena *a, Ty *t) {
  Buf b = {0};
  ty_print(&b, t);
  char *s = arena_cstr(a, str_from(b.p ? b.p : "", b.len));
  buf_free(&b);
  return s;
}

bool ty_has_param(Ty *t) {
  if (!t) return false;
  switch (t->kind) {
    case TY_PARAM: return true;
    case TY_LIST: case TY_OPTION: case TY_STORE: return ty_has_param(t->a);
    case TY_MAP: case TY_RESULT: case TY_ENTRY: return ty_has_param(t->a) || ty_has_param(t->b);
    case TY_STRUCT: case TY_ENUM:
      for (int i = 0; i < t->args.len; i++) if (ty_has_param(t->args.items[i])) return true;
      return false;
    case TY_FN:
      for (int i = 0; i < t->params.len; i++) if (ty_has_param(t->params.items[i])) return true;
      return ty_has_param(t->ret);
    default: return false;
  }
}

static int param_index(StrVec *names, Str name) {
  for (int i = 0; i < names->len; i++) if (str_eq(names->items[i], name)) return i;
  return -1;
}

Ty *ty_subst(Arena *a, Ty *t, StrVec *names, TyVec *binds) {
  if (!t || !names || names->len == 0) return t;
  switch (t->kind) {
    case TY_PARAM: {
      int i = param_index(names, t->name);
      if (i >= 0 && i < binds->len && binds->items[i]) return binds->items[i];
      return t;
    }
    case TY_LIST: return ty_list(a, ty_subst(a, t->a, names, binds));
    case TY_OPTION: return ty_option(a, ty_subst(a, t->a, names, binds));
    case TY_STORE: return ty_store(a, ty_subst(a, t->a, names, binds));
    case TY_MAP: return ty_map(a, ty_subst(a, t->a, names, binds), ty_subst(a, t->b, names, binds));
    case TY_RESULT: return ty_result(a, ty_subst(a, t->a, names, binds), ty_subst(a, t->b, names, binds));
    case TY_ENTRY: return ty_entry(a, ty_subst(a, t->a, names, binds), ty_subst(a, t->b, names, binds));
    case TY_STRUCT: case TY_ENUM: {
      if (!t->args.len) return t;
      Ty *n = ty_named(a, t->kind, t->name, t->decl);
      for (int i = 0; i < t->args.len; i++) { Ty *s = ty_subst(a, t->args.items[i], names, binds); VEC_PUSH(a, &n->args, s); }
      return n;
    }
    case TY_FN: {
      TyVec ps = {0};
      for (int i = 0; i < t->params.len; i++) { Ty *s = ty_subst(a, t->params.items[i], names, binds); VEC_PUSH(a, &ps, s); }
      return ty_fn(a, ps, ty_subst(a, t->ret, names, binds));
    }
    default: return t;
  }
}

bool ty_unify(Arena *a, Ty *pat, Ty *actual, StrVec *names, TyVec *binds) {
  if (!pat || !actual) return true;
  if (actual->kind == TY_ERR || actual->kind == TY_NEVER) return true;
  if (pat->kind == TY_PARAM && names) {
    int i = param_index(names, pat->name);
    if (i >= 0) {
      while (binds->len <= i) VEC_PUSH(a, binds, (Ty *)NULL);
      if (!binds->items[i]) { binds->items[i] = actual; return true; }
      return ty_eq(binds->items[i], actual);
    }
  }
  if (pat->kind != actual->kind) return false;
  switch (pat->kind) {
    case TY_LIST: case TY_OPTION: case TY_STORE: return ty_unify(a, pat->a, actual->a, names, binds);
    case TY_MAP: case TY_RESULT: case TY_ENTRY:
      return ty_unify(a, pat->a, actual->a, names, binds) && ty_unify(a, pat->b, actual->b, names, binds);
    case TY_STRUCT: case TY_ENUM:
      if (pat->decl != actual->decl || pat->args.len != actual->args.len) return false;
      for (int i = 0; i < pat->args.len; i++)
        if (!ty_unify(a, pat->args.items[i], actual->args.items[i], names, binds)) return false;
      return true;
    case TY_FN:
      if (pat->params.len != actual->params.len) return false;
      for (int i = 0; i < pat->params.len; i++)
        if (!ty_unify(a, pat->params.items[i], actual->params.items[i], names, binds)) return false;
      return ty_unify(a, pat->ret, actual->ret, names, binds);
    default: return ty_eq(pat, actual);
  }
}
