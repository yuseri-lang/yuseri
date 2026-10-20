// Yuseri Stage 0: semantic types
#ifndef YU_TYPES_H
#define YU_TYPES_H

#include "ast.h"

typedef enum {
  TY_INT, TY_FLOAT, TY_BOOL, TY_STRING, TY_BYTES, TY_UNIT,
  TY_NEVER,   // panic / return: compatible with everything
  TY_LIST,    // a = element
  TY_MAP,     // a = key, b = value
  TY_OPTION,  // a = value
  TY_RESULT,  // a = ok, b = err
  TY_ENTRY,   // a = key, b = value (from Map.entries())
  TY_STRUCT,  // decl (+ args for generic structs)
  TY_ENUM,    // decl (NULL for builtin enums such as ParseError)
  TY_FN,      // params, ret
  TY_PARAM,   // generic parameter named `name`
  TY_STORE,   // a = value type (layer 2 Store<T>)
  TY_CAP,     // capability named `name`: Net, Fs, Env, Clock, Random, Log, Tasks
  TY_ERR,     // unknown after an error: compatible with everything, never reported again
} TyKind;

typedef struct Ty Ty;
typedef VEC(Ty *) TyVec;

struct Ty {
  TyKind kind;
  Str name;     // TY_STRUCT, TY_ENUM, TY_PARAM
  Decl *decl;   // TY_STRUCT, TY_ENUM
  Ty *a, *b;
  TyVec params; // TY_FN
  Ty *ret;      // TY_FN
  TyVec args;   // TY_STRUCT / TY_ENUM generic arguments
};

Ty *ty_prim(TyKind k);  // shared singletons for the primitive kinds, TY_NEVER and TY_ERR
Ty *ty_list(Arena *a, Ty *elem);
Ty *ty_map(Arena *a, Ty *k, Ty *v);
Ty *ty_option(Arena *a, Ty *v);
Ty *ty_result(Arena *a, Ty *ok, Ty *err);
Ty *ty_entry(Arena *a, Ty *k, Ty *v);
Ty *ty_store(Arena *a, Ty *v);
Ty *ty_fn(Arena *a, TyVec params, Ty *ret);
Ty *ty_param(Arena *a, Str name);
Ty *ty_named(Arena *a, TyKind k, Str name, Decl *decl);

bool ty_is_err(Ty *t);
bool ty_eq(Ty *x, Ty *y);  // TY_ERR and TY_NEVER are compatible with anything
void ty_print(Buf *b, Ty *t);
const char *ty_str(Arena *a, Ty *t);
bool ty_has_param(Ty *t);
// Replaces TY_PARAM names[i] with binds[i] (NULL binds are left as parameters).
Ty *ty_subst(Arena *a, Ty *t, StrVec *names, TyVec *binds);
// Binds parameters in `pat` so that it matches `actual`. Returns false on a mismatch.
bool ty_unify(Arena *a, Ty *pat, Ty *actual, StrVec *names, TyVec *binds);

#endif
