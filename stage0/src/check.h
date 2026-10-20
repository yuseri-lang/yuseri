// Yuseri Stage 0: name resolution and type checking
#ifndef YU_CHECK_H
#define YU_CHECK_H

#include "ast.h"

// Checks a parsed module and reports every E- error through diag_error.
void check_module(Arena *a, Module *m);

#endif
