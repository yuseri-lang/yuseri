// Yuseri Stage 0: C code generation from the type-checked module
#ifndef YU_CGEN_H
#define YU_CGEN_H

#include "ast.h"

// Writes a complete C translation unit for `m` into `out`. The module must have passed
// check_module without errors. With `tests` the unit's main runs the test blocks.
void cgen_module(Arena *a, Module *m, bool tests, Buf *out);

#endif
