// Yuseri Stage 0 runtime: JSON text <-> a generic tree. Typed encoders and decoders are
// generated per type by the compiler on top of these primitives.
#ifndef YU_JSON_H
#define YU_JSON_H

#include "yu_rt.h"

enum { YU_JV_NULL, YU_JV_BOOL, YU_JV_NUMBER, YU_JV_STRING, YU_JV_ARRAY, YU_JV_OBJECT };

typedef struct yu_jv yu_jv;
typedef struct { yu_str key; yu_jv *val; } yu_jkv;

struct yu_jv {
  int kind;
  bool b;        // YU_JV_BOOL
  yu_str text;   // YU_JV_NUMBER: the digits as written; YU_JV_STRING: unescaped
  yu_list items; // YU_JV_ARRAY: yu_jv *; YU_JV_OBJECT: yu_jkv
};

// Parses `text`. On failure returns false and sets *err_at to the byte offset.
bool yu_json_parse(yu_str text, yu_jv **out, int64_t *err_at);
yu_jv *yu_jv_get(yu_jv *obj, yu_str key);        // NULL when missing
bool yu_jv_int(yu_jv *v, int64_t *out);          // number without fraction / exponent
bool yu_jv_float(yu_jv *v, double *out);
void yu_json_quote(yu_sb *b, yu_str s);          // "..." with JSON escapes
const char *yu_jv_kind_name(yu_jv *v);

// Decoder error: the path (`users[2].id`) and the expected type name.
typedef struct { yu_str path; const char *expected; } yu_json_err;
void yu_json_path_push(yu_sb *path, const char *seg, int64_t index);  // seg, or [index] when seg is NULL

#endif
