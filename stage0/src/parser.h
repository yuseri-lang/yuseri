// Yuseri Stage 0: parser
#ifndef YU_PARSER_H
#define YU_PARSER_H

#include "ast.h"

// Parses a whole module. Syntax errors are reported through diag_error; the returned
// module may be partial when diag_count() > 0.
Module *parse_module(Arena *a, const char *file, LexResult lx);

#endif
