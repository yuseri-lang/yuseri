// Yuseri Stage 0: formatter (canonical printer)
#ifndef YU_FMT_H
#define YU_FMT_H

#include "ast.h"

// Prints the module in canonical layout into `out`. Comments and single blank lines
// inside blocks are preserved; everything else is recomputed.
void fmt_module(Module *m, Buf *out);

#endif
