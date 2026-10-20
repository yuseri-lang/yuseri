// Yuseri Stage 0: syntax tree
#ifndef YU_AST_H
#define YU_AST_H

#include "common.h"
#include "lexer.h"

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Block Block;
typedef struct Pattern Pattern;
typedef struct Decl Decl;
struct Ty;  // semantic type (types.h); filled in by the checker
typedef VEC(struct Ty *) SemTyVec;
typedef struct Param Param;
typedef struct FieldInit FieldInit;
typedef struct PatField PatField;
typedef struct MatchArm MatchArm;
typedef struct Variant Variant;

typedef VEC(Type *) TypeVec;
typedef VEC(Expr *) ExprVec;
typedef VEC(Stmt *) StmtVec;
typedef VEC(Decl *) DeclVec;
typedef VEC(Str) StrVec;
typedef VEC(Param) ParamVec;
typedef VEC(FieldInit) FieldInitVec;
typedef VEC(PatField) PatFieldVec;
typedef VEC(MatchArm) MatchArmVec;
typedef VEC(Variant) VariantVec;

typedef enum { TYPE_NAMED, TYPE_FN } TypeKind;
struct Type {
  TypeKind kind;
  int line;
  Str name;          // TYPE_NAMED
  TypeVec args;      // TYPE_NAMED type arguments, TYPE_FN parameter types
  Type *ret;         // TYPE_FN
};

struct Param {
  Str name;
  Type *type;  // NULL only for `self`
  int line;
  struct Ty *ty;  // resolved by the checker
};

struct FieldInit {
  Str name;
  Expr *value;
};

typedef enum {
  PAT_WILD,     // _
  PAT_BIND,     // name
  PAT_LIT,      // literal expression (int, string, bool)
  PAT_VARIANT,  // Path(fields) e.g. Some(x), Status.Banned(reason: r), None
} PatKind;

struct PatField {
  Str name;      // field name (empty for positional, e.g. Some(x))
  Pattern *pat;
};

struct Pattern {
  PatKind kind;
  int line;
  Str name;             // PAT_BIND
  Expr *lit;            // PAT_LIT
  StrVec path;          // PAT_VARIANT: e.g. ["Status", "Banned"] or ["Some"]
  PatFieldVec fields;   // PAT_VARIANT
  bool has_parens;      // PAT_VARIANT: written with (...)
  struct Ty *ty;        // type of the matched value (checker)
};

struct MatchArm {
  Pattern *pat;
  Expr *guard;  // may be NULL
  Expr *body;   // expression or block expression
  int line;
};

typedef enum {
  E_INT, E_FLOAT, E_BOOL, E_STRING, E_IDENT,
  E_FIELD,    // obj.name
  E_INDEX,    // obj[idx]
  E_CALL,     // callee(args)
  E_UNARY,    // op e
  E_BINARY,   // l op r
  E_TRY,      // e?
  E_IF,       // if cond block else block|if
  E_MATCH,
  E_STRUCT,   // Name { fields } or Name { ..base, fields }
  E_LIST,     // [items]
  E_FN,       // fn(params) -> ret block
  E_BLOCK,    // { ... } used as a value
  E_TYPECTOR, // List<Int>() / Map<K, V>()  -> stored as type + call
  E_PAREN,    // kept only to preserve explicit grouping? no: fmt recomputes. unused
} ExprKind;

struct Expr {
  ExprKind kind;
  int line;
  Str text;             // literals, identifiers, field names, operators
  TokKind op;           // E_UNARY, E_BINARY
  Expr *a, *b;          // operands / callee / object / condition
  ExprVec args;         // E_CALL args, E_LIST items
  StrVec arg_names;     // E_CALL: parallel to args; empty Str for a positional argument
  StrVec segs;          // E_STRING: literal pieces (raw, escapes kept); segs.len == args.len + 1
  Block *block;         // E_IF then, E_FN body, E_BLOCK
  Block *else_block;    // E_IF else (NULL if none)
  Expr *else_if;        // E_IF: else-if chain (an E_IF), NULL if none
  MatchArmVec arms;     // E_MATCH
  Str type_name;        // E_STRUCT
  FieldInitVec fields;  // E_STRUCT
  Expr *base;           // E_STRUCT ..base
  ParamVec params;      // E_FN
  Type *ret;            // E_FN
  Type *type;           // E_TYPECTOR
  bool bool_val;
  struct Ty *ty;        // type of the expression (checker)
  StrVec cap_names;     // E_FN: captured outer bindings, by value
  SemTyVec cap_types;   // E_FN: their types
  int id;               // E_FN: unique number for the generated C function
  Decl *method;         // E_CALL: resolved impl method, if any
  SemTyVec type_args;   // E_CALL: bound generic arguments of a generic fn / method, in declaration order
  TypeVec explicit_types; // E_FIELD callee: type arguments written at the call, e.g. json.decode<User>(s)
  int call_kind;        // E_CALL: see CallKind
};

typedef enum {
  CK_UNKNOWN,
  CK_FN,        // user function by name
  CK_VALUE,     // function value (local, parameter, captured)
  CK_METHOD,    // impl method: callee is E_FIELD(obj, name)
  CK_BUILTIN,   // builtin method on List / Map / Option / Result / String / enum
  CK_STATIC,    // Int.parse, Float.from, String.from
  CK_VARIANT,   // Enum.Variant(fields)
  CK_PRELUDE,   // Some / Ok / Err / assert / assert_eq / panic
  CK_MODULE,    // std module function: json.encode, http.serve, ...
} CallKind;

typedef enum { S_LET, S_ASSIGN, S_EXPR, S_RETURN, S_BREAK, S_CONTINUE, S_FOR, S_WHILE } StmtKind;

struct Stmt {
  StmtKind kind;
  int line, end_line;
  bool mutable;    // S_LET: var
  Str name;        // S_LET, S_FOR loop variable
  Expr *target;    // S_ASSIGN
  Expr *value;     // S_LET, S_ASSIGN, S_EXPR, S_RETURN (may be NULL), S_FOR iterable, S_WHILE cond
  Block *body;     // S_FOR, S_WHILE
  struct Ty *ty;   // S_LET: type of the binding; S_FOR: element type (checker)
};

struct Block {
  StmtVec stmts;
  int line, end_line;
};

struct Variant {
  Str name;
  ParamVec fields;  // enum variant fields (name: Type)
  int line;
};

typedef enum { D_FN, D_STRUCT, D_ENUM, D_IMPL, D_IMPORT, D_TEST, D_CONST } DeclKind;

struct Decl {
  DeclKind kind;
  int line, end_line;
  bool pub;
  Str name;             // fn/struct/enum/impl target/test name/const name
  StrVec generics;      // fn, struct, enum
  ParamVec params;      // fn params, struct fields
  Type *ret;            // fn return type (NULL = missing), const type
  Block *body;          // fn, test
  VariantVec variants;  // enum
  DeclVec methods;      // impl
  StrVec path;          // import path segments
  Expr *value;          // const
  struct Ty *ty;        // fn: function type; const: value type (checker)
  bool from_std;        // declared by a std module (std.http Request / Response, std.json JsonError)
};

typedef struct {
  DeclVec decls;
  CommentVec comments;
  const char *file;
} Module;

#endif
