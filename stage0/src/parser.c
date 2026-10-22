#include "parser.h"

typedef struct {
  Arena *a;
  const char *file;
  Token *t;
  int i, n;
  int no_struct;  // >0 while parsing a condition: `Name {` is not a struct literal
} P;

static Token *cur(P *p) { return &p->t[p->i]; }
static TokKind kind(P *p) { return p->t[p->i].kind; }
static TokKind kind_at(P *p, int k) { return p->i + k < p->n ? p->t[p->i + k].kind : T_EOF; }
static bool at(P *p, TokKind k) { return kind(p) == k; }
static int prev_line(P *p) { return p->i > 0 ? p->t[p->i - 1].line : 1; }

static Token *advance(P *p) {
  Token *t = cur(p);
  if (p->i < p->n - 1) p->i++;
  return t;
}

static bool accept(P *p, TokKind k) {
  if (at(p, k)) { advance(p); return true; }
  return false;
}

static void error_here(P *p, const char *what) {
  Token *t = cur(p);
  if (t->kind == T_DOTDOT)
    diag_error("E-SYN-01", p->file, t->line, "expected %s, got `..` (a range belongs only in `for i in a..b`)", what);
  else if (t->kind == T_IDENT || t->kind == T_INT || t->kind == T_FLOAT)
    diag_error("E-SYN-01", p->file, t->line, "expected %s, got `%.*s`", what, t->text.len, t->text.p);
  else
    diag_error("E-SYN-01", p->file, t->line, "expected %s, got %s", what, tok_name(t->kind));
}

static Token *expect(P *p, TokKind k, const char *what) {
  if (at(p, k)) return advance(p);
  error_here(p, what);
  return NULL;
}

static void skip_newlines(P *p) { while (at(p, T_NEWLINE)) advance(p); }

// Skips to the end of the current line (or past a closing brace at depth 0) after an error.
static void sync(P *p) {
  int depth = 0;
  while (!at(p, T_EOF)) {
    TokKind k = kind(p);
    if (k == T_LBRACE) depth++;
    if (k == T_RBRACE) { if (depth == 0) { advance(p); return; } depth--; }
    if (k == T_NEWLINE && depth == 0) { advance(p); return; }
    advance(p);
  }
}

static Str ident(P *p) {
  Token *t = expect(p, T_IDENT, "identifier");
  return t ? t->text : STR_LIT("_");
}

static bool is_upper(Str s) { return s.len > 0 && s.p[0] >= 'A' && s.p[0] <= 'Z'; }

// ---- types -------------------------------------------------------------

static Type *parse_type(P *p);

static Type *new_type(P *p, TypeKind k) {
  Type *t = NEW(p->a, Type);
  t->kind = k;
  t->line = cur(p)->line;
  return t;
}

static Type *parse_type(P *p) {
  if (at(p, T_FN)) {
    Type *t = new_type(p, TYPE_FN);
    advance(p);
    expect(p, T_LPAREN, "`(`");
    while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
      Type *arg = parse_type(p);
      VEC_PUSH(p->a, &t->args, arg);
      if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "`)`");
    expect(p, T_ARROW, "`->`");
    t->ret = parse_type(p);
    return t;
  }
  Type *t = new_type(p, TYPE_NAMED);
  t->name = ident(p);
  if (accept(p, T_LT)) {
    while (!at(p, T_GT) && !at(p, T_EOF)) {
      Type *arg = parse_type(p);
      VEC_PUSH(p->a, &t->args, arg);
      if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_GT, "`>`");
  }
  return t;
}

// ---- expressions -------------------------------------------------------

static Expr *parse_expr(P *p);
static Block *parse_block(P *p);
static Pattern *parse_pattern(P *p);

static Expr *new_expr(P *p, ExprKind k, int line) {
  Expr *e = NEW(p->a, Expr);
  e->kind = k;
  e->line = line;
  return e;
}

static void parse_params(P *p, ParamVec *out) {
  expect(p, T_LPAREN, "`(`");
  skip_newlines(p);
  while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
    Param prm = {0};
    prm.line = cur(p)->line;
    prm.name = ident(p);
    if (str_eq_c(prm.name, "self")) {
      prm.type = NULL;
    } else {
      expect(p, T_COLON, "`:`");
      prm.type = parse_type(p);
    }
    VEC_PUSH(p->a, out, prm);
    skip_newlines(p);
    if (!accept(p, T_COMMA)) break;
    skip_newlines(p);
  }
  skip_newlines(p);
  expect(p, T_RPAREN, "`)`");
}

static Expr *parse_if(P *p) {
  Expr *e = new_expr(p, E_IF, cur(p)->line);
  expect(p, T_IF, "`if`");
  p->no_struct++;
  e->a = parse_expr(p);
  p->no_struct--;
  e->block = parse_block(p);
  if (accept(p, T_ELSE)) {
    if (at(p, T_IF)) e->else_if = parse_if(p);
    else e->else_block = parse_block(p);
  }
  return e;
}

static Expr *parse_match(P *p) {
  Expr *e = new_expr(p, E_MATCH, cur(p)->line);
  expect(p, T_MATCH, "`match`");
  p->no_struct++;
  e->a = parse_expr(p);
  p->no_struct--;
  expect(p, T_LBRACE, "`{`");
  skip_newlines(p);
  while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
    MatchArm arm = {0};
    arm.line = cur(p)->line;
    arm.pat = parse_pattern(p);
    if (accept(p, T_IF)) {
      p->no_struct++;
      arm.guard = parse_expr(p);
      p->no_struct--;
    }
    expect(p, T_FATARROW, "`=>`");
    if (at(p, T_LBRACE)) {
      Expr *b = new_expr(p, E_BLOCK, cur(p)->line);
      b->block = parse_block(p);
      arm.body = b;
    } else {
      arm.body = parse_expr(p);
    }
    VEC_PUSH(p->a, &e->arms, arm);
    if (!at(p, T_RBRACE)) {
      if (!at(p, T_NEWLINE)) { error_here(p, "newline or `}` after match arm"); sync(p); }
      skip_newlines(p);
    }
  }
  expect(p, T_RBRACE, "`}`");
  return e;
}

static Expr *parse_fn_expr(P *p) {
  Expr *e = new_expr(p, E_FN, cur(p)->line);
  expect(p, T_FN, "`fn`");
  parse_params(p, &e->params);
  if (accept(p, T_ARROW)) e->ret = parse_type(p);
  e->block = parse_block(p);
  return e;
}

static Expr *parse_struct_lit(P *p, Str name, int line) {
  Expr *e = new_expr(p, E_STRUCT, line);
  e->type_name = name;
  expect(p, T_LBRACE, "`{`");
  skip_newlines(p);
  if (accept(p, T_DOTDOT)) {
    e->base = parse_expr(p);
    skip_newlines(p);
    if (!accept(p, T_COMMA)) { skip_newlines(p); expect(p, T_RBRACE, "`}`"); return e; }
    skip_newlines(p);
  }
  while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
    FieldInit f = {0};
    f.name = ident(p);
    expect(p, T_COLON, "`:`");
    f.value = parse_expr(p);
    VEC_PUSH(p->a, &e->fields, f);
    skip_newlines(p);
    if (!accept(p, T_COMMA)) break;
    skip_newlines(p);
  }
  skip_newlines(p);
  expect(p, T_RBRACE, "`}`");
  return e;
}

// Parses `a, b` or `name: a, name: b` (named arguments build enum variants) up to `close`.
static void parse_args(P *p, ExprVec *out, StrVec *names, TokKind close) {
  int saved = p->no_struct;
  p->no_struct = 0;
  skip_newlines(p);
  while (!at(p, close) && !at(p, T_EOF)) {
    Str name = {0};
    if (names && at(p, T_IDENT) && kind_at(p, 1) == T_COLON) {
      name = ident(p);
      advance(p);
    }
    Expr *arg = parse_expr(p);
    VEC_PUSH(p->a, out, arg);
    if (names) VEC_PUSH(p->a, names, name);
    skip_newlines(p);
    if (!accept(p, T_COMMA)) break;
    skip_newlines(p);
  }
  skip_newlines(p);
  expect(p, close, close == T_RPAREN ? "`)`" : "`]`");
  p->no_struct = saved;
}

// Splits a raw string literal into literal segments and embedded expressions.
// `{expr}` is parsed with a sub-lexer; `\{` and `\}` stay in the segment text.
static void parse_string_parts(P *p, Expr *e, Str raw, int line) {
  int seg_start = 0;
  int i = 0;
  while (i < raw.len) {
    char c = raw.p[i];
    if (c == '\\') { i += 2; continue; }
    if (c == '{') {
      int depth = 1, j = i + 1;
      while (j < raw.len && depth > 0) {
        if (raw.p[j] == '{') depth++;
        else if (raw.p[j] == '}') depth--;
        if (depth > 0) j++;
      }
      Str seg = str_from(raw.p + seg_start, i - seg_start);
      VEC_PUSH(p->a, &e->segs, seg);
      Str inner = str_from(raw.p + i + 1, j - i - 1);
      LexResult lx = lex_at(p->a, p->file, inner, line);
      P sub = {.a = p->a, .file = p->file, .t = lx.toks.items, .i = 0, .n = lx.toks.len};
      Expr *x = parse_expr(&sub);
      if (sub.t[sub.i].kind != T_NEWLINE && sub.t[sub.i].kind != T_EOF)
        diag_error("E-SYN-01", p->file, line, "unexpected %s inside interpolation", tok_name(sub.t[sub.i].kind));
      VEC_PUSH(p->a, &e->args, x);
      i = j + 1;
      seg_start = i;
      continue;
    }
    i++;
  }
  Str last = str_from(raw.p + seg_start, raw.len - seg_start);
  VEC_PUSH(p->a, &e->segs, last);
}

// True when the tokens at `<` look like generic arguments followed by `(`:
// `List<Int>(`, `Map<String, fn(Int) -> Int>(`. `MAX < 200` is a comparison.
static bool looks_like_type_args(P *p) {
  int i = p->i;
  if (p->t[i].kind != T_LT) return false;
  int depth = 0;
  for (; i < p->n; i++) {
    switch (p->t[i].kind) {
      case T_LT: depth++; break;
      case T_GT:
        depth--;
        if (depth == 0) return i + 1 < p->n && p->t[i + 1].kind == T_LPAREN;
        break;
      case T_IDENT: case T_COMMA: case T_FN: case T_LPAREN: case T_RPAREN: case T_ARROW: break;
      default: return false;
    }
  }
  return false;
}

static Expr *parse_primary(P *p) {
  Token *t = cur(p);
  switch (t->kind) {
    case T_INT: { advance(p); Expr *e = new_expr(p, E_INT, t->line); e->text = t->text; return e; }
    case T_FLOAT: { advance(p); Expr *e = new_expr(p, E_FLOAT, t->line); e->text = t->text; return e; }
    case T_STRING: {
      advance(p);
      Expr *e = new_expr(p, E_STRING, t->line);
      e->text = t->text;
      parse_string_parts(p, e, t->text, t->line);
      return e;
    }
    case T_TRUE: case T_FALSE: {
      advance(p);
      Expr *e = new_expr(p, E_BOOL, t->line);
      e->bool_val = t->kind == T_TRUE;
      e->text = t->text;
      return e;
    }
    case T_IDENT: {
      advance(p);
      if (is_upper(t->text)) {
        if (at(p, T_LBRACE) && p->no_struct == 0) return parse_struct_lit(p, t->text, t->line);
        if (at(p, T_LT) && looks_like_type_args(p)) {
          // generic constructor: List<Int>() / Map<K, V>()
          Expr *e = new_expr(p, E_TYPECTOR, t->line);
          Type *ty = NEW(p->a, Type);
          ty->kind = TYPE_NAMED;
          ty->line = t->line;
          ty->name = t->text;
          advance(p);
          while (!at(p, T_GT) && !at(p, T_EOF)) {
            Type *arg = parse_type(p);
            VEC_PUSH(p->a, &ty->args, arg);
            if (!accept(p, T_COMMA)) break;
          }
          expect(p, T_GT, "`>`");
          e->type = ty;
          expect(p, T_LPAREN, "`(`");
          expect(p, T_RPAREN, "`)`");
          return e;
        }
      }
      Expr *e = new_expr(p, E_IDENT, t->line);
      e->text = t->text;
      return e;
    }
    case T_LPAREN: {
      advance(p);
      int saved = p->no_struct;
      p->no_struct = 0;
      skip_newlines(p);
      Expr *e = parse_expr(p);
      skip_newlines(p);
      expect(p, T_RPAREN, "`)`");
      p->no_struct = saved;
      return e;
    }
    case T_LBRACKET: {
      advance(p);
      Expr *e = new_expr(p, E_LIST, t->line);
      parse_args(p, &e->args, NULL, T_RBRACKET);
      return e;
    }
    case T_IF: return parse_if(p);
    case T_MATCH: return parse_match(p);
    case T_FN: return parse_fn_expr(p);
    default:
      error_here(p, "an expression");
      advance(p);
      return new_expr(p, E_IDENT, t->line);
  }
}

static Expr *parse_postfix(P *p) {
  Expr *e = parse_primary(p);
  for (;;) {
    Token *t = cur(p);
    if (t->kind == T_DOT) {
      advance(p);
      Expr *f = new_expr(p, E_FIELD, t->line);
      f->a = e;
      f->text = ident(p);
      if (at(p, T_LT) && looks_like_type_args(p)) {  // json.decode<User>(s)
        advance(p);
        while (!at(p, T_GT) && !at(p, T_EOF)) {
          Type *ty = parse_type(p);
          VEC_PUSH(p->a, &f->explicit_types, ty);
          if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_GT, "`>`");
      }
      e = f;
    } else if (t->kind == T_LPAREN) {
      advance(p);
      Expr *c = new_expr(p, E_CALL, t->line);
      c->a = e;
      parse_args(p, &c->args, &c->arg_names, T_RPAREN);
      e = c;
    } else if (t->kind == T_LBRACKET) {
      advance(p);
      Expr *x = new_expr(p, E_INDEX, t->line);
      x->a = e;
      int saved = p->no_struct;
      p->no_struct = 0;
      x->b = parse_expr(p);
      p->no_struct = saved;
      expect(p, T_RBRACKET, "`]`");
      e = x;
    } else if (t->kind == T_QUESTION) {
      advance(p);
      Expr *q = new_expr(p, E_TRY, t->line);
      q->a = e;
      e = q;
    } else {
      return e;
    }
  }
}

static Expr *parse_unary(P *p) {
  Token *t = cur(p);
  if (t->kind == T_MINUS || t->kind == T_BANG) {
    advance(p);
    Expr *e = new_expr(p, E_UNARY, t->line);
    e->op = t->kind;
    e->a = parse_unary(p);
    return e;
  }
  return parse_postfix(p);
}

static Expr *binary(P *p, TokKind op, Expr *l, Expr *r, int line) {
  Expr *e = new_expr(p, E_BINARY, line);
  e->op = op;
  e->a = l;
  e->b = r;
  return e;
}

static Expr *parse_mul(P *p) {
  Expr *l = parse_unary(p);
  while (at(p, T_STAR) || at(p, T_SLASH) || at(p, T_PERCENT)) {
    Token *t = advance(p);
    l = binary(p, t->kind, l, parse_unary(p), t->line);
  }
  return l;
}

static Expr *parse_add(P *p) {
  Expr *l = parse_mul(p);
  while (at(p, T_PLUS) || at(p, T_MINUS)) {
    Token *t = advance(p);
    l = binary(p, t->kind, l, parse_mul(p), t->line);
  }
  return l;
}

static bool is_cmp(TokKind k) { return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE; }

static Expr *parse_cmp(P *p) {
  Expr *l = parse_add(p);
  if (is_cmp(kind(p))) {
    Token *t = advance(p);
    l = binary(p, t->kind, l, parse_add(p), t->line);
  }
  return l;
}

static Expr *parse_and(P *p) {
  Expr *l = parse_cmp(p);
  while (at(p, T_ANDAND)) {
    Token *t = advance(p);
    l = binary(p, t->kind, l, parse_cmp(p), t->line);
  }
  return l;
}

static Expr *parse_or(P *p) {
  Expr *l = parse_and(p);
  while (at(p, T_OROR)) {
    Token *t = advance(p);
    l = binary(p, t->kind, l, parse_and(p), t->line);
  }
  return l;
}

// A range `a..b` is not an expression: only `for` reads it (parse_stmt), so `..`
// anywhere else is E-SYN-01 (core.md section 9).
static Expr *parse_expr(P *p) { return parse_or(p); }

// ---- patterns ----------------------------------------------------------

static Pattern *parse_pattern(P *p) {
  Pattern *pat = NEW(p->a, Pattern);
  Token *t = cur(p);
  pat->line = t->line;
  if (t->kind == T_IDENT && str_eq_c(t->text, "_")) {
    advance(p);
    pat->kind = PAT_WILD;
    return pat;
  }
  if (t->kind == T_INT || t->kind == T_STRING || t->kind == T_TRUE || t->kind == T_FALSE || t->kind == T_MINUS) {
    pat->kind = PAT_LIT;
    pat->lit = parse_unary(p);
    return pat;
  }
  Str name = ident(p);
  if (!is_upper(name)) {
    pat->kind = PAT_BIND;
    pat->name = name;
    return pat;
  }
  pat->kind = PAT_VARIANT;
  VEC_PUSH(p->a, &pat->path, name);
  while (accept(p, T_DOT)) {
    Str seg = ident(p);
    VEC_PUSH(p->a, &pat->path, seg);
  }
  if (accept(p, T_LPAREN)) {
    pat->has_parens = true;
    while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
      PatField f = {0};
      if (at(p, T_IDENT) && kind_at(p, 1) == T_COLON) {
        f.name = ident(p);
        advance(p);
      }
      f.pat = parse_pattern(p);
      VEC_PUSH(p->a, &pat->fields, f);
      if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN, "`)`");
  }
  return pat;
}

// ---- statements and blocks ---------------------------------------------

static Stmt *new_stmt(P *p, StmtKind k, int line) {
  Stmt *s = NEW(p->a, Stmt);
  s->kind = k;
  s->line = line;
  return s;
}

static Stmt *parse_stmt(P *p) {
  Token *t = cur(p);
  Stmt *s;
  switch (t->kind) {
    case T_LET: case T_VAR: {
      advance(p);
      s = new_stmt(p, S_LET, t->line);
      s->mutable = t->kind == T_VAR;
      s->name = ident(p);
      expect(p, T_ASSIGN, "`=`");
      s->value = parse_expr(p);
      break;
    }
    case T_RETURN: {
      advance(p);
      s = new_stmt(p, S_RETURN, t->line);
      if (!at(p, T_NEWLINE) && !at(p, T_RBRACE)) s->value = parse_expr(p);
      break;
    }
    case T_BREAK: advance(p); s = new_stmt(p, S_BREAK, t->line); break;
    case T_CONTINUE: advance(p); s = new_stmt(p, S_CONTINUE, t->line); break;
    case T_FOR: {
      advance(p);
      s = new_stmt(p, S_FOR, t->line);
      s->name = ident(p);
      expect(p, T_IN, "`in`");
      p->no_struct++;
      Expr *lo = parse_expr(p);
      if (at(p, T_DOTDOT)) {  // range a..b: allowed only here
        Token *dd = advance(p);
        s->value = binary(p, T_DOTDOT, lo, parse_expr(p), dd->line);
      } else {
        s->value = lo;
      }
      p->no_struct--;
      s->body = parse_block(p);
      break;
    }
    case T_WHILE: {
      advance(p);
      s = new_stmt(p, S_WHILE, t->line);
      p->no_struct++;
      s->value = parse_expr(p);
      p->no_struct--;
      s->body = parse_block(p);
      break;
    }
    default: {
      Expr *e = parse_expr(p);
      if (at(p, T_ASSIGN)) {
        advance(p);
        s = new_stmt(p, S_ASSIGN, t->line);
        s->target = e;
        s->value = parse_expr(p);
      } else {
        s = new_stmt(p, S_EXPR, t->line);
        s->value = e;
      }
    }
  }
  s->end_line = prev_line(p);
  return s;
}

static Block *parse_block(P *p) {
  Block *b = NEW(p->a, Block);
  b->line = cur(p)->line;
  if (!expect(p, T_LBRACE, "`{`")) { b->end_line = b->line; return b; }
  skip_newlines(p);
  while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
    int before = p->i;
    Stmt *s = parse_stmt(p);
    VEC_PUSH(p->a, &b->stmts, s);
    if (at(p, T_NEWLINE)) skip_newlines(p);
    else if (!at(p, T_RBRACE)) { error_here(p, "newline or `}`"); sync(p); }
    if (p->i == before) advance(p);
  }
  Token *close = expect(p, T_RBRACE, "`}`");
  b->end_line = close ? close->line : prev_line(p);
  return b;
}

// ---- declarations ------------------------------------------------------

static Decl *new_decl(P *p, DeclKind k, int line) {
  Decl *d = NEW(p->a, Decl);
  d->kind = k;
  d->line = line;
  return d;
}

static void parse_generics(P *p, StrVec *out) {
  if (!accept(p, T_LT)) return;
  while (!at(p, T_GT) && !at(p, T_EOF)) {
    Str g = ident(p);
    VEC_PUSH(p->a, out, g);
    if (!accept(p, T_COMMA)) break;
  }
  expect(p, T_GT, "`>`");
}

static void parse_field_list(P *p, ParamVec *out) {
  expect(p, T_LBRACE, "`{`");
  skip_newlines(p);
  while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
    Param f = {0};
    f.line = cur(p)->line;
    f.name = ident(p);
    expect(p, T_COLON, "`:`");
    f.type = parse_type(p);
    VEC_PUSH(p->a, out, f);
    skip_newlines(p);
    if (!accept(p, T_COMMA)) break;
    skip_newlines(p);
  }
  skip_newlines(p);
  expect(p, T_RBRACE, "`}`");
}

static Decl *parse_fn_decl(P *p, bool pub) {
  Decl *d = new_decl(p, D_FN, cur(p)->line);
  d->pub = pub;
  expect(p, T_FN, "`fn`");
  d->name = ident(p);
  parse_generics(p, &d->generics);
  parse_params(p, &d->params);
  if (accept(p, T_ARROW)) d->ret = parse_type(p);
  d->body = parse_block(p);
  d->end_line = prev_line(p);
  return d;
}

static Decl *parse_decl(P *p) {
  Token *t = cur(p);
  bool pub = accept(p, T_PUB);
  Decl *d = NULL;
  switch (kind(p)) {
    case T_FN: d = parse_fn_decl(p, pub); return d;
    case T_STRUCT: {
      d = new_decl(p, D_STRUCT, t->line);
      advance(p);
      d->name = ident(p);
      parse_generics(p, &d->generics);
      parse_field_list(p, &d->params);
      break;
    }
    case T_ENUM: {
      d = new_decl(p, D_ENUM, t->line);
      advance(p);
      d->name = ident(p);
      parse_generics(p, &d->generics);
      expect(p, T_LBRACE, "`{`");
      skip_newlines(p);
      while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        Variant v = {0};
        v.line = cur(p)->line;
        v.name = ident(p);
        if (at(p, T_LPAREN)) {
          advance(p);
          while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
            Param f = {0};
            f.line = cur(p)->line;
            f.name = ident(p);
            expect(p, T_COLON, "`:`");
            f.type = parse_type(p);
            VEC_PUSH(p->a, &v.fields, f);
            if (!accept(p, T_COMMA)) break;
          }
          expect(p, T_RPAREN, "`)`");
        }
        VEC_PUSH(p->a, &d->variants, v);
        skip_newlines(p);
        if (!accept(p, T_COMMA)) break;
        skip_newlines(p);
      }
      skip_newlines(p);
      expect(p, T_RBRACE, "`}`");
      break;
    }
    case T_IMPL: {
      d = new_decl(p, D_IMPL, t->line);
      advance(p);
      d->name = ident(p);
      expect(p, T_LBRACE, "`{`");
      skip_newlines(p);
      while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        int before = p->i;
        bool mpub = accept(p, T_PUB);
        if (at(p, T_FN)) {
          Decl *m = parse_fn_decl(p, mpub);
          VEC_PUSH(p->a, &d->methods, m);
        } else {
          error_here(p, "`fn` inside impl");
          sync(p);
        }
        skip_newlines(p);
        if (p->i == before) advance(p);
      }
      expect(p, T_RBRACE, "`}`");
      break;
    }
    case T_IMPORT: {
      d = new_decl(p, D_IMPORT, t->line);
      advance(p);
      Str seg = ident(p);
      VEC_PUSH(p->a, &d->path, seg);
      while (accept(p, T_DOT)) {
        seg = ident(p);
        VEC_PUSH(p->a, &d->path, seg);
      }
      break;
    }
    case T_TEST: {
      d = new_decl(p, D_TEST, t->line);
      advance(p);
      Token *name = expect(p, T_STRING, "test name");
      d->name = name ? name->text : STR_LIT("");
      d->body = parse_block(p);
      break;
    }
    case T_CONST: {
      d = new_decl(p, D_CONST, t->line);
      advance(p);
      d->name = ident(p);
      expect(p, T_COLON, "`:`");
      d->ret = parse_type(p);
      expect(p, T_ASSIGN, "`=`");
      d->value = parse_expr(p);
      break;
    }
    default:
      error_here(p, "a declaration (`fn`, `struct`, `enum`, `impl`, `import`, `test`, `const`)");
      sync(p);
      return NULL;
  }
  d->pub = pub;
  d->end_line = prev_line(p);
  return d;
}

Module *parse_module(Arena *a, const char *file, LexResult lx) {
  P ps = {.a = a, .file = file, .t = lx.toks.items, .i = 0, .n = lx.toks.len};
  P *p = &ps;
  Module *m = NEW(a, Module);
  m->file = file;
  m->comments = lx.comments;
  for (;;) {
    skip_newlines(p);
    if (at(p, T_EOF)) break;
    int before = p->i;
    Decl *d = parse_decl(p);
    if (d) {
      VEC_PUSH(a, &m->decls, d);
      if (!at(p, T_NEWLINE) && !at(p, T_EOF)) { error_here(p, "newline after declaration"); sync(p); }
    }
    if (p->i == before) advance(p);
  }
  return m;
}
