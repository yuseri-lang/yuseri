#include "lexer.h"

static const struct {
  const char *word;
  TokKind kind;
} keywords[] = {
    {"fn", T_FN},         {"let", T_LET},       {"var", T_VAR},     {"const", T_CONST},
    {"if", T_IF},         {"else", T_ELSE},     {"for", T_FOR},     {"in", T_IN},
    {"while", T_WHILE},   {"break", T_BREAK},   {"continue", T_CONTINUE},
    {"return", T_RETURN}, {"struct", T_STRUCT}, {"enum", T_ENUM},   {"impl", T_IMPL},
    {"import", T_IMPORT}, {"pub", T_PUB},       {"match", T_MATCH}, {"test", T_TEST},
    {"true", T_TRUE},     {"false", T_FALSE},
};

static const char *names[T_COUNT] = {
    [T_EOF] = "end of file", [T_NEWLINE] = "newline", [T_IDENT] = "identifier",
    [T_INT] = "integer", [T_FLOAT] = "float", [T_STRING] = "string",
    [T_FN] = "fn", [T_LET] = "let", [T_VAR] = "var", [T_CONST] = "const", [T_IF] = "if",
    [T_ELSE] = "else", [T_FOR] = "for", [T_IN] = "in", [T_WHILE] = "while",
    [T_BREAK] = "break", [T_CONTINUE] = "continue", [T_RETURN] = "return",
    [T_STRUCT] = "struct", [T_ENUM] = "enum", [T_IMPL] = "impl", [T_IMPORT] = "import",
    [T_PUB] = "pub", [T_MATCH] = "match", [T_TEST] = "test", [T_TRUE] = "true",
    [T_FALSE] = "false", [T_LPAREN] = "(", [T_RPAREN] = ")", [T_LBRACE] = "{",
    [T_RBRACE] = "}", [T_LBRACKET] = "[", [T_RBRACKET] = "]", [T_COMMA] = ",",
    [T_COLON] = ":", [T_DOT] = ".", [T_DOTDOT] = "..", [T_ARROW] = "->",
    [T_FATARROW] = "=>", [T_QUESTION] = "?", [T_BANG] = "!", [T_ASSIGN] = "=",
    [T_EQ] = "==", [T_NE] = "!=", [T_LT] = "<", [T_LE] = "<=", [T_GT] = ">",
    [T_GE] = ">=", [T_PLUS] = "+", [T_MINUS] = "-", [T_STAR] = "*", [T_SLASH] = "/",
    [T_PERCENT] = "%", [T_ANDAND] = "&&", [T_OROR] = "||",
};

const char *tok_name(TokKind k) { return names[k] ? names[k] : "?"; }

// A newline after one of these tokens continues the statement (core.md section 1).
static bool continues_line(TokKind k) {
  switch (k) {
    case T_LPAREN: case T_LBRACKET: case T_LBRACE: case T_COMMA: case T_FATARROW:
    case T_ARROW: case T_ASSIGN: case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT:
    case T_GE: case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
    case T_ANDAND: case T_OROR: case T_DOTDOT: case T_COLON: case T_DOT:
      return true;
    default:
      return false;
  }
}

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

typedef struct {
  Arena *a;
  const char *file;
  Str src;
  int i, line, col;
  bool code_on_line;
  LexResult r;
} Lexer;

static char peek(Lexer *L, int k) { return L->i + k < L->src.len ? L->src.p[L->i + k] : 0; }

static void push(Lexer *L, TokKind kind, int start, int line, int col) {
  Token t = {kind, str_from(L->src.p + start, L->i - start), line, col};
  VEC_PUSH(L->a, &L->r.toks, t);
  L->code_on_line = true;
}

static void newline(Lexer *L) {
  int n = L->r.toks.len;
  if (n > 0 && L->r.toks.items[n - 1].kind != T_NEWLINE && !continues_line(L->r.toks.items[n - 1].kind)) {
    Token t = {T_NEWLINE, str_from(L->src.p + L->i, 1), L->line, L->col};
    VEC_PUSH(L->a, &L->r.toks, t);
  }
  L->i++;
  L->line++;
  L->col = 1;
  L->code_on_line = false;
}

// Scans a string literal body starting after the opening quote. Reports E-STR-01 for a bare
// brace and E-SYN-02 for an unterminated literal. Returns the index of the closing quote.
static int scan_string(Lexer *L, int start_line) {
  int depth = 0;
  while (L->i < L->src.len) {
    char c = L->src.p[L->i];
    if (c == '\n') {
      diag_error("E-SYN-02", L->file, start_line, "unterminated string literal");
      return L->i;
    }
    if (c == '\\') {
      L->i += 2;
      L->col += 2;
      continue;
    }
    if (depth == 0) {
      if (c == '"') return L->i;
      if (c == '{') depth = 1;
      else if (c == '}') diag_error("E-STR-01", L->file, L->line, "bare `}` in string literal");
    } else {
      if (c == '{') depth++;
      else if (c == '}') depth--;
      else if (c == '"') {  // nested string inside an interpolation: skip to its end
        L->i++;
        L->col++;
        while (L->i < L->src.len && L->src.p[L->i] != '"' && L->src.p[L->i] != '\n') {
          if (L->src.p[L->i] == '\\') { L->i++; L->col++; }
          L->i++;
          L->col++;
        }
      }
    }
    L->i++;
    L->col++;
  }
  diag_error("E-SYN-02", L->file, start_line, "unterminated string literal");
  return L->i;
}

LexResult lex(Arena *a, const char *file, Str src) { return lex_at(a, file, src, 1); }

LexResult lex_at(Arena *a, const char *file, Str src, int base_line) {
  Lexer Lx = {.a = a, .file = file, .src = src, .i = 0, .line = base_line, .col = 1};
  Lexer *L = &Lx;
  while (L->i < src.len) {
    char c = src.p[L->i];
    if (c == '\n') { newline(L); continue; }
    if (c == ' ' || c == '\t' || c == '\r') { L->i++; L->col++; continue; }
    if (c == '/' && peek(L, 1) == '/') {
      int start = L->i + 2;
      while (L->i < src.len && src.p[L->i] != '\n') L->i++;
      Comment cm = {str_from(src.p + start, L->i - start), L->line, L->code_on_line};
      VEC_PUSH(a, &L->r.comments, cm);
      continue;
    }
    int start = L->i, line = L->line, col = L->col;
    if (is_ident_start(c)) {
      while (is_ident_char(peek(L, 0))) { L->i++; L->col++; }
      Str word = str_from(src.p + start, L->i - start);
      TokKind k = T_IDENT;
      for (size_t j = 0; j < sizeof keywords / sizeof keywords[0]; j++)
        if (str_eq_c(word, keywords[j].word)) { k = keywords[j].kind; break; }
      push(L, k, start, line, col);
      continue;
    }
    if (is_digit(c)) {
      while (is_digit(peek(L, 0))) { L->i++; L->col++; }
      TokKind k = T_INT;
      if (peek(L, 0) == '.' && is_digit(peek(L, 1))) {
        k = T_FLOAT;
        L->i++; L->col++;
        while (is_digit(peek(L, 0))) { L->i++; L->col++; }
      }
      push(L, k, start, line, col);
      continue;
    }
    if (c == '"') {
      L->i++; L->col++;
      int body = L->i;
      int end = scan_string(L, line);
      Token t = {T_STRING, str_from(src.p + body, end - body), line, col};
      VEC_PUSH(a, &L->r.toks, t);
      L->code_on_line = true;
      if (L->i < src.len && src.p[L->i] == '"') { L->i++; L->col++; }
      continue;
    }
    TokKind k = T_EOF;
    int n = 1;
    char d = peek(L, 1);
    switch (c) {
      case '(': k = T_LPAREN; break;
      case ')': k = T_RPAREN; break;
      case '{': k = T_LBRACE; break;
      case '}': k = T_RBRACE; break;
      case '[': k = T_LBRACKET; break;
      case ']': k = T_RBRACKET; break;
      case ',': k = T_COMMA; break;
      case ':': k = T_COLON; break;
      case '?': k = T_QUESTION; break;
      case '+': k = T_PLUS; break;
      case '*': k = T_STAR; break;
      case '/': k = T_SLASH; break;
      case '%': k = T_PERCENT; break;
      case '.': if (d == '.') { k = T_DOTDOT; n = 2; } else k = T_DOT; break;
      case '-': if (d == '>') { k = T_ARROW; n = 2; } else k = T_MINUS; break;
      case '=': if (d == '=') { k = T_EQ; n = 2; } else if (d == '>') { k = T_FATARROW; n = 2; } else k = T_ASSIGN; break;
      case '!': if (d == '=') { k = T_NE; n = 2; } else k = T_BANG; break;
      case '<': if (d == '=') { k = T_LE; n = 2; } else k = T_LT; break;
      case '>': if (d == '=') { k = T_GE; n = 2; } else k = T_GT; break;
      case '&': if (d == '&') { k = T_ANDAND; n = 2; } break;
      case '|': if (d == '|') { k = T_OROR; n = 2; } break;
      default: break;
    }
    if (k == T_EOF) {
      diag_error("E-SYN-01", file, line, "unexpected character `%c`", c);
      L->i++; L->col++;
      continue;
    }
    L->i += n;
    L->col += n;
    push(L, k, start, line, col);
  }
  int n = L->r.toks.len;
  if (n > 0 && L->r.toks.items[n - 1].kind != T_NEWLINE) {
    Token t = {T_NEWLINE, str_from(src.p + src.len, 0), L->line, L->col};
    VEC_PUSH(a, &L->r.toks, t);
  }
  Token eof = {T_EOF, str_from(src.p + src.len, 0), L->line, L->col};
  VEC_PUSH(a, &L->r.toks, eof);
  return L->r;
}
