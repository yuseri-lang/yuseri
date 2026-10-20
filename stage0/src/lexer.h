// Yuseri Stage 0: lexer
#ifndef YU_LEXER_H
#define YU_LEXER_H

#include "common.h"

typedef enum {
  T_EOF,
  T_NEWLINE,
  T_IDENT,
  T_INT,
  T_FLOAT,
  T_STRING,  // text = raw contents between the quotes
  // keywords
  T_FN, T_LET, T_VAR, T_CONST, T_IF, T_ELSE, T_FOR, T_IN, T_WHILE, T_BREAK, T_CONTINUE,
  T_RETURN, T_STRUCT, T_ENUM, T_IMPL, T_IMPORT, T_PUB, T_MATCH, T_TEST, T_TRUE, T_FALSE,
  // punctuation
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
  T_COMMA, T_COLON, T_DOT, T_DOTDOT, T_ARROW, T_FATARROW, T_QUESTION, T_BANG,
  T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_ANDAND, T_OROR,
  T_COUNT
} TokKind;

typedef struct {
  TokKind kind;
  Str text;
  int line;
  int col;
} Token;

typedef struct {
  Str text;      // without the leading "//"
  int line;
  bool trailing; // true when code precedes it on the same line
} Comment;

typedef VEC(Token) TokenVec;
typedef VEC(Comment) CommentVec;

typedef struct {
  TokenVec toks;
  CommentVec comments;
} LexResult;

// Tokenizes `src`. Newlines that continue a statement are dropped. Errors are reported
// through diag_error and lexing continues.
LexResult lex(Arena *a, const char *file, Str src);
LexResult lex_at(Arena *a, const char *file, Str src, int base_line);
const char *tok_name(TokKind k);

#endif
