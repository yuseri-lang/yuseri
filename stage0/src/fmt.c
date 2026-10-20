#include "fmt.h"

#define WIDTH 80

typedef struct {
  Buf *out;
  Module *m;
  int ci;         // next comment index
  int indent;
  int last_line;  // last source line whose content was printed
  bool inline_mode;
  bool inline_failed;
} F;

static void fmt_expr(F *f, Expr *e, int min_prec);
static void fmt_block(F *f, Block *b);
static void fmt_type(F *f, Type *t);

static int cur_col(F *f) {
  int i = f->out->len;
  while (i > 0 && f->out->p[i - 1] != '\n') i--;
  return f->out->len - i;
}

static void nl(F *f) { buf_push(f->out, '\n'); }
static void ind(F *f) { for (int i = 0; i < f->indent; i++) buf_puts(f->out, "  "); }

static void put_comment(F *f, Comment *c) {
  Str t = c->text;
  while (t.len > 0 && (t.p[t.len - 1] == ' ' || t.p[t.len - 1] == '\t' || t.p[t.len - 1] == '\r')) t.len--;
  buf_puts(f->out, "//");
  if (t.len > 0 && t.p[0] != ' ') buf_push(f->out, ' ');
  buf_str(f->out, t);
}

static bool comments_in(F *f, int from, int to) {
  for (int i = f->ci; i < f->m->comments.len; i++) {
    int l = f->m->comments.items[i].line;
    if (l > to) break;
    if (l >= from) return true;
  }
  return false;
}

// Prints own-line comments that appear before `line`, keeping single blank lines.
static void comments_before(F *f, int line, bool allow_blank) {
  while (f->ci < f->m->comments.len && f->m->comments.items[f->ci].line < line) {
    Comment *c = &f->m->comments.items[f->ci];
    if (allow_blank && f->last_line > 0 && c->line > f->last_line + 1) nl(f);
    ind(f);
    put_comment(f, c);
    nl(f);
    f->last_line = c->line;
    f->ci++;
  }
}

// Appends trailing comments that belong to source lines up to `end_line` (inclusive).
static void trailing_comments(F *f, int end_line) {
  while (f->ci < f->m->comments.len && f->m->comments.items[f->ci].line <= end_line) {
    buf_puts(f->out, "  ");
    put_comment(f, &f->m->comments.items[f->ci]);
    f->ci++;
  }
}

// ---- types -------------------------------------------------------------

static void fmt_type(F *f, Type *t) {
  if (t->kind == TYPE_FN) {
    buf_puts(f->out, "fn(");
    for (int i = 0; i < t->args.len; i++) {
      if (i) buf_puts(f->out, ", ");
      fmt_type(f, t->args.items[i]);
    }
    buf_puts(f->out, ") -> ");
    fmt_type(f, t->ret);
    return;
  }
  buf_str(f->out, t->name);
  if (t->args.len) {
    buf_push(f->out, '<');
    for (int i = 0; i < t->args.len; i++) {
      if (i) buf_puts(f->out, ", ");
      fmt_type(f, t->args.items[i]);
    }
    buf_push(f->out, '>');
  }
}

static void fmt_params(F *f, ParamVec *ps) {
  buf_push(f->out, '(');
  for (int i = 0; i < ps->len; i++) {
    if (i) buf_puts(f->out, ", ");
    buf_str(f->out, ps->items[i].name);
    if (ps->items[i].type) {
      buf_puts(f->out, ": ");
      fmt_type(f, ps->items[i].type);
    }
  }
  buf_push(f->out, ')');
}

// ---- inline attempts ---------------------------------------------------

// Renders with `render` into a temporary buffer in inline mode. Returns true and appends
// the text when the whole thing fits on the current line without multi-line constructs.
typedef void (*RenderFn)(F *f, void *arg);

static bool try_inline(F *f, RenderFn render, void *arg) {
  if (f->inline_mode) {  // already inline: just render, failure propagates
    render(f, arg);
    return !f->inline_failed;
  }
  Buf tmp = {0};
  F g = *f;
  g.out = &tmp;
  g.inline_mode = true;
  g.inline_failed = false;
  render(&g, arg);
  bool ok = !g.inline_failed && cur_col(f) + tmp.len <= WIDTH && memchr(tmp.p, '\n', (size_t)tmp.len) == NULL;
  if (ok) buf_write(f->out, tmp.p, tmp.len);
  buf_free(&tmp);
  return ok;
}

static bool expr_has_block(Expr *e) {
  if (!e) return false;
  switch (e->kind) {
    case E_IF: case E_MATCH: case E_FN: case E_BLOCK: return true;
    case E_FIELD: case E_TRY: case E_UNARY: case E_INDEX: return expr_has_block(e->a) || expr_has_block(e->b);
    case E_BINARY: return expr_has_block(e->a) || expr_has_block(e->b);
    case E_CALL: {
      if (expr_has_block(e->a)) return true;
      for (int i = 0; i < e->args.len; i++) if (expr_has_block(e->args.items[i])) return true;
      return false;
    }
    case E_LIST:
      for (int i = 0; i < e->args.len; i++) if (expr_has_block(e->args.items[i])) return true;
      return false;
    case E_STRUCT:
      if (expr_has_block(e->base)) return true;
      for (int i = 0; i < e->fields.len; i++) if (expr_has_block(e->fields.items[i].value)) return true;
      return false;
    default: return false;
  }
}

static bool block_inline_ok(F *f, Block *b) {
  if (b->stmts.len > 1) return false;
  if (comments_in(f, b->line, b->end_line)) return false;
  if (b->stmts.len == 1) {
    Stmt *s = b->stmts.items[0];
    if (s->kind == S_FOR || s->kind == S_WHILE) return false;
    if (expr_has_block(s->value) || expr_has_block(s->target)) return false;
  }
  return true;
}

// ---- expressions -------------------------------------------------------

static int bin_prec(TokKind op) {
  switch (op) {
    case T_DOTDOT: return 1;
    case T_OROR: return 2;
    case T_ANDAND: return 3;
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: return 4;
    case T_PLUS: case T_MINUS: return 5;
    case T_STAR: case T_SLASH: case T_PERCENT: return 6;
    default: return 0;
  }
}

static int expr_prec(Expr *e) {
  switch (e->kind) {
    case E_BINARY: return bin_prec(e->op);
    case E_UNARY: return 7;
    case E_FIELD: case E_INDEX: case E_CALL: case E_TRY: return 8;
    case E_IF: case E_MATCH: case E_FN: case E_BLOCK: return 0;
    default: return 9;
  }
}

static void fmt_stmt(F *f, Stmt *s);

static void render_block_inline(F *f, void *arg) {
  Block *b = arg;
  if (b->stmts.len == 0) { buf_puts(f->out, "{ }"); return; }
  buf_puts(f->out, "{ ");
  fmt_stmt(f, b->stmts.items[0]);
  buf_puts(f->out, " }");
}

static void fmt_block_multi(F *f, Block *b);

static void fmt_block(F *f, Block *b) {
  if (block_inline_ok(f, b) && try_inline(f, render_block_inline, b)) {
    f->last_line = b->end_line;
    return;
  }
  fmt_block_multi(f, b);
}

// Always prints the block form, one statement per line.
static void fmt_block_multi(F *f, Block *b) {
  if (f->inline_mode) { f->inline_failed = true; return; }
  if (b->stmts.len == 0 && !comments_in(f, b->line, b->end_line)) { buf_puts(f->out, "{ }"); f->last_line = b->end_line; return; }
  buf_puts(f->out, "{");
  nl(f);
  f->indent++;
  f->last_line = b->line;
  for (int i = 0; i < b->stmts.len; i++) {
    Stmt *s = b->stmts.items[i];
    comments_before(f, s->line, i > 0);
    if (i > 0 && s->line > f->last_line + 1) nl(f);
    ind(f);
    fmt_stmt(f, s);
    trailing_comments(f, s->end_line);
    nl(f);
    f->last_line = s->end_line;
  }
  comments_before(f, b->end_line, true);
  f->indent--;
  ind(f);
  buf_puts(f->out, "}");
  f->last_line = b->end_line;
}

typedef struct { Expr **items; Str *names; int len; } ExprList;

static void put_name(F *f, Str *names, int i) {
  if (names && names[i].len) { buf_str(f->out, names[i]); buf_puts(f->out, ": "); }
}

static void render_list(F *f, void *arg) {
  ExprList *l = arg;
  for (int i = 0; i < l->len; i++) {
    if (i) buf_puts(f->out, ", ");
    put_name(f, l->names, i);
    fmt_expr(f, l->items[i], 0);
  }
}

// Prints `open items close`, breaking one item per line when too wide.
static void fmt_list(F *f, Expr **items, Str *names, int len, const char *open, const char *close) {
  buf_puts(f->out, open);
  ExprList l = {items, names, len};
  if (len == 0 || try_inline(f, render_list, &l)) { buf_puts(f->out, close); return; }
  if (f->inline_mode) { f->inline_failed = true; return; }
  nl(f);
  f->indent++;
  for (int i = 0; i < len; i++) {
    ind(f);
    put_name(f, names, i);
    fmt_expr(f, items[i], 0);
    buf_push(f->out, ',');
    nl(f);
  }
  f->indent--;
  ind(f);
  buf_puts(f->out, close);
}

static void render_struct_fields(F *f, void *arg) {
  Expr *e = arg;
  bool first = true;
  if (e->base) { buf_puts(f->out, ".."); fmt_expr(f, e->base, 0); first = false; }
  for (int i = 0; i < e->fields.len; i++) {
    if (!first) buf_puts(f->out, ", ");
    first = false;
    buf_str(f->out, e->fields.items[i].name);
    buf_puts(f->out, ": ");
    fmt_expr(f, e->fields.items[i].value, 0);
  }
}

static void fmt_struct_lit(F *f, Expr *e) {
  buf_str(f->out, e->type_name);
  buf_puts(f->out, " { ");
  if (!e->base && e->fields.len == 0) { buf_puts(f->out, "}"); return; }
  if (try_inline(f, render_struct_fields, e)) { buf_puts(f->out, " }"); return; }
  if (f->inline_mode) { f->inline_failed = true; return; }
  f->out->len--;  // drop the space after "{"
  nl(f);
  f->indent++;
  if (e->base) { ind(f); buf_puts(f->out, ".."); fmt_expr(f, e->base, 0); buf_push(f->out, ','); nl(f); }
  for (int i = 0; i < e->fields.len; i++) {
    ind(f);
    buf_str(f->out, e->fields.items[i].name);
    buf_puts(f->out, ": ");
    fmt_expr(f, e->fields.items[i].value, 0);
    buf_push(f->out, ',');
    nl(f);
  }
  f->indent--;
  ind(f);
  buf_puts(f->out, "}");
}

static void fmt_pattern(F *f, Pattern *p) {
  switch (p->kind) {
    case PAT_WILD: buf_puts(f->out, "_"); break;
    case PAT_BIND: buf_str(f->out, p->name); break;
    case PAT_LIT: fmt_expr(f, p->lit, 0); break;
    case PAT_VARIANT:
      for (int i = 0; i < p->path.len; i++) {
        if (i) buf_push(f->out, '.');
        buf_str(f->out, p->path.items[i]);
      }
      if (p->has_parens) {
        buf_push(f->out, '(');
        for (int i = 0; i < p->fields.len; i++) {
          if (i) buf_puts(f->out, ", ");
          if (p->fields.items[i].name.len) { buf_str(f->out, p->fields.items[i].name); buf_puts(f->out, ": "); }
          fmt_pattern(f, p->fields.items[i].pat);
        }
        buf_push(f->out, ')');
      }
      break;
  }
}

static void render_if_chain(F *f, void *arg);

static void fmt_if(F *f, Expr *e) {
  if (!f->inline_mode && try_inline(f, render_if_chain, e)) return;
  if (f->inline_mode) { f->inline_failed = true; return; }
  // multi-line chain: every block is printed in block form so the chain stays uniform
  Expr *cur = e;
  for (;;) {
    buf_puts(f->out, "if ");
    fmt_expr(f, cur->a, 0);
    buf_push(f->out, ' ');
    fmt_block_multi(f, cur->block);
    if (cur->else_if) { buf_puts(f->out, " else "); cur = cur->else_if; continue; }
    if (cur->else_block) { buf_puts(f->out, " else "); fmt_block_multi(f, cur->else_block); }
    break;
  }
}

static void render_if_chain(F *f, void *arg) {
  Expr *cur = arg;
  for (;;) {
    buf_puts(f->out, "if ");
    fmt_expr(f, cur->a, 0);
    buf_push(f->out, ' ');
    if (!block_inline_ok(f, cur->block)) { f->inline_failed = true; return; }
    render_block_inline(f, cur->block);
    if (cur->else_if) { buf_puts(f->out, " else "); cur = cur->else_if; continue; }
    if (cur->else_block) {
      buf_puts(f->out, " else ");
      if (!block_inline_ok(f, cur->else_block)) { f->inline_failed = true; return; }
      render_block_inline(f, cur->else_block);
    }
    return;
  }
}

static void fmt_match(F *f, Expr *e) {
  if (f->inline_mode) { f->inline_failed = true; return; }
  buf_puts(f->out, "match ");
  fmt_expr(f, e->a, 0);
  buf_puts(f->out, " {");
  nl(f);
  f->indent++;
  for (int i = 0; i < e->arms.len; i++) {
    MatchArm *arm = &e->arms.items[i];
    comments_before(f, arm->line, i > 0);
    ind(f);
    fmt_pattern(f, arm->pat);
    if (arm->guard) { buf_puts(f->out, " if "); fmt_expr(f, arm->guard, 0); }
    buf_puts(f->out, " => ");
    Expr *body = arm->body;
    int end = arm->line;
    if (body->kind == E_BLOCK) {
      Block *b = body->block;
      end = b->end_line;
      if (b->stmts.len == 1 && b->stmts.items[0]->kind == S_EXPR && block_inline_ok(f, b)) {
        Expr *only = b->stmts.items[0]->value;
        Buf tmp = {0};
        F g = *f; g.out = &tmp; g.inline_mode = true; g.inline_failed = false;
        fmt_expr(&g, only, 0);
        if (!g.inline_failed && cur_col(f) + tmp.len <= WIDTH) { buf_write(f->out, tmp.p, tmp.len); buf_free(&tmp); }
        else { buf_free(&tmp); fmt_block(f, b); }
      } else {
        fmt_block(f, b);
      }
    } else {
      fmt_expr(f, body, 0);
    }
    trailing_comments(f, end);
    nl(f);
    f->last_line = end;
  }
  f->indent--;
  ind(f);
  buf_puts(f->out, "}");
}

static void fmt_fn_expr(F *f, Expr *e) {
  buf_puts(f->out, "fn");
  fmt_params(f, &e->params);
  if (e->ret) { buf_puts(f->out, " -> "); fmt_type(f, e->ret); }
  buf_push(f->out, ' ');
  fmt_block(f, e->block);
}

static void fmt_expr(F *f, Expr *e, int min_prec) {
  int prec = expr_prec(e);
  bool parens = prec < min_prec;
  if (parens) buf_push(f->out, '(');
  switch (e->kind) {
    case E_INT: case E_FLOAT: case E_IDENT: buf_str(f->out, e->text); break;
    case E_BOOL: buf_puts(f->out, e->bool_val ? "true" : "false"); break;
    case E_STRING: buf_push(f->out, '"'); buf_str(f->out, e->text); buf_push(f->out, '"'); break;
    case E_FIELD: fmt_expr(f, e->a, 8); buf_push(f->out, '.'); buf_str(f->out, e->text); break;
    case E_INDEX: fmt_expr(f, e->a, 8); buf_push(f->out, '['); fmt_expr(f, e->b, 0); buf_push(f->out, ']'); break;
    case E_TRY: fmt_expr(f, e->a, 8); buf_push(f->out, '?'); break;
    case E_CALL: fmt_expr(f, e->a, 8); fmt_list(f, e->args.items, e->arg_names.items, e->args.len, "(", ")"); break;
    case E_LIST: fmt_list(f, e->args.items, NULL, e->args.len, "[", "]"); break;
    case E_UNARY: buf_puts(f->out, e->op == T_MINUS ? "-" : "!"); fmt_expr(f, e->a, 7); break;
    case E_BINARY: {
      int p = bin_prec(e->op);
      fmt_expr(f, e->a, p);
      if (e->op == T_DOTDOT) buf_puts(f->out, "..");
      else { buf_push(f->out, ' '); buf_puts(f->out, tok_name(e->op)); buf_push(f->out, ' '); }
      fmt_expr(f, e->b, p + 1);
      break;
    }
    case E_TYPECTOR: fmt_type(f, e->type); buf_puts(f->out, "()"); break;
    case E_STRUCT: fmt_struct_lit(f, e); break;
    case E_IF: fmt_if(f, e); break;
    case E_MATCH: fmt_match(f, e); break;
    case E_FN: fmt_fn_expr(f, e); break;
    case E_BLOCK: fmt_block(f, e->block); break;
    case E_PAREN: break;
  }
  if (parens) buf_push(f->out, ')');
}

// ---- statements --------------------------------------------------------

static void fmt_stmt(F *f, Stmt *s) {
  switch (s->kind) {
    case S_LET:
      buf_puts(f->out, s->mutable ? "var " : "let ");
      buf_str(f->out, s->name);
      buf_puts(f->out, " = ");
      fmt_expr(f, s->value, 0);
      break;
    case S_ASSIGN: fmt_expr(f, s->target, 0); buf_puts(f->out, " = "); fmt_expr(f, s->value, 0); break;
    case S_EXPR: fmt_expr(f, s->value, 0); break;
    case S_RETURN:
      buf_puts(f->out, "return");
      if (s->value) { buf_push(f->out, ' '); fmt_expr(f, s->value, 0); }
      break;
    case S_BREAK: buf_puts(f->out, "break"); break;
    case S_CONTINUE: buf_puts(f->out, "continue"); break;
    case S_FOR:
      if (f->inline_mode) { f->inline_failed = true; return; }
      buf_puts(f->out, "for ");
      buf_str(f->out, s->name);
      buf_puts(f->out, " in ");
      fmt_expr(f, s->value, 0);
      buf_push(f->out, ' ');
      fmt_block_multi(f, s->body);
      break;
    case S_WHILE:
      if (f->inline_mode) { f->inline_failed = true; return; }
      buf_puts(f->out, "while ");
      fmt_expr(f, s->value, 0);
      buf_push(f->out, ' ');
      fmt_block_multi(f, s->body);
      break;
  }
}

// ---- declarations ------------------------------------------------------

static void fmt_generics(F *f, StrVec *g) {
  if (!g->len) return;
  buf_push(f->out, '<');
  for (int i = 0; i < g->len; i++) {
    if (i) buf_puts(f->out, ", ");
    buf_str(f->out, g->items[i]);
  }
  buf_push(f->out, '>');
}

static void fmt_fn_decl(F *f, Decl *d) {
  if (d->pub) buf_puts(f->out, "pub ");
  buf_puts(f->out, "fn ");
  buf_str(f->out, d->name);
  fmt_generics(f, &d->generics);
  fmt_params(f, &d->params);
  if (d->ret) { buf_puts(f->out, " -> "); fmt_type(f, d->ret); }
  buf_push(f->out, ' ');
  fmt_block(f, d->body);
}

typedef struct { ParamVec *fields; } FieldArg;

static void render_fields(F *f, void *arg) {
  ParamVec *ps = ((FieldArg *)arg)->fields;
  for (int i = 0; i < ps->len; i++) {
    if (i) buf_puts(f->out, ", ");
    buf_str(f->out, ps->items[i].name);
    buf_puts(f->out, ": ");
    fmt_type(f, ps->items[i].type);
  }
}

static void fmt_struct_decl(F *f, Decl *d) {
  if (d->pub) buf_puts(f->out, "pub ");
  buf_puts(f->out, "struct ");
  buf_str(f->out, d->name);
  fmt_generics(f, &d->generics);
  buf_puts(f->out, " { ");
  FieldArg fa = {&d->params};
  if (d->params.len == 0) { buf_puts(f->out, "}"); return; }
  if (!comments_in(f, d->line, d->end_line) && try_inline(f, render_fields, &fa)) { buf_puts(f->out, " }"); return; }
  f->out->len--;
  nl(f);
  f->indent++;
  for (int i = 0; i < d->params.len; i++) {
    Param *p = &d->params.items[i];
    comments_before(f, p->line, false);
    ind(f);
    buf_str(f->out, p->name);
    buf_puts(f->out, ": ");
    fmt_type(f, p->type);
    buf_push(f->out, ',');
    trailing_comments(f, p->line);
    nl(f);
    f->last_line = p->line;
  }
  comments_before(f, d->end_line, false);
  f->indent--;
  buf_puts(f->out, "}");
}

static void render_variants(F *f, void *arg) {
  Decl *d = arg;
  for (int i = 0; i < d->variants.len; i++) {
    Variant *v = &d->variants.items[i];
    if (i) buf_puts(f->out, ", ");
    buf_str(f->out, v->name);
    if (v->fields.len) {
      FieldArg fa = {&v->fields};
      buf_push(f->out, '(');
      render_fields(f, &fa);
      buf_push(f->out, ')');
    }
  }
}

static void fmt_enum_decl(F *f, Decl *d) {
  if (d->pub) buf_puts(f->out, "pub ");
  buf_puts(f->out, "enum ");
  buf_str(f->out, d->name);
  fmt_generics(f, &d->generics);
  buf_puts(f->out, " { ");
  if (d->variants.len == 0) { buf_puts(f->out, "}"); return; }
  if (!comments_in(f, d->line, d->end_line) && try_inline(f, render_variants, d)) { buf_puts(f->out, " }"); return; }
  f->out->len--;
  nl(f);
  f->indent++;
  for (int i = 0; i < d->variants.len; i++) {
    Variant *v = &d->variants.items[i];
    comments_before(f, v->line, false);
    ind(f);
    buf_str(f->out, v->name);
    if (v->fields.len) {
      FieldArg fa = {&v->fields};
      buf_push(f->out, '(');
      render_fields(f, &fa);
      buf_push(f->out, ')');
    }
    buf_push(f->out, ',');
    trailing_comments(f, v->line);
    nl(f);
    f->last_line = v->line;
  }
  comments_before(f, d->end_line, false);
  f->indent--;
  buf_puts(f->out, "}");
}

static void fmt_decl(F *f, Decl *d) {
  switch (d->kind) {
    case D_FN: fmt_fn_decl(f, d); break;
    case D_STRUCT: fmt_struct_decl(f, d); break;
    case D_ENUM: fmt_enum_decl(f, d); break;
    case D_IMPL:
      buf_puts(f->out, "impl ");
      buf_str(f->out, d->name);
      buf_puts(f->out, " {");
      nl(f);
      f->indent++;
      f->last_line = d->line;
      for (int i = 0; i < d->methods.len; i++) {
        Decl *m = d->methods.items[i];
        comments_before(f, m->line, i > 0);
        if (i > 0) nl(f);
        ind(f);
        fmt_fn_decl(f, m);
        trailing_comments(f, m->end_line);
        nl(f);
        f->last_line = m->end_line;
      }
      comments_before(f, d->end_line, true);
      f->indent--;
      buf_puts(f->out, "}");
      break;
    case D_IMPORT:
      buf_puts(f->out, "import ");
      for (int i = 0; i < d->path.len; i++) {
        if (i) buf_push(f->out, '.');
        buf_str(f->out, d->path.items[i]);
      }
      break;
    case D_TEST:
      buf_puts(f->out, "test \"");
      buf_str(f->out, d->name);
      buf_puts(f->out, "\" ");
      fmt_block(f, d->body);
      break;
    case D_CONST:
      if (d->pub) buf_puts(f->out, "pub ");
      buf_puts(f->out, "const ");
      buf_str(f->out, d->name);
      buf_puts(f->out, ": ");
      fmt_type(f, d->ret);
      buf_puts(f->out, " = ");
      fmt_expr(f, d->value, 0);
      break;
  }
}

void fmt_module(Module *m, Buf *out) {
  F f = {.out = out, .m = m};
  for (int i = 0; i < m->decls.len; i++) {
    Decl *d = m->decls.items[i];
    bool imports = i > 0 && d->kind == D_IMPORT && m->decls.items[i - 1]->kind == D_IMPORT;
    if (i > 0 && !imports) nl(&f);
    comments_before(&f, d->line, false);
    fmt_decl(&f, d);
    trailing_comments(&f, d->end_line);
    nl(&f);
    f.last_line = d->end_line;
  }
  comments_before(&f, 1 << 30, true);
}
