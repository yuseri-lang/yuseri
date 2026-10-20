// Yuseri Stage 0: building and running generated C
#ifndef YU_DRIVER_H
#define YU_DRIVER_H

#include "common.h"

// Compiles a .yu file to an executable. `tests` builds the test runner instead of main.
// Returns the path of the executable (arena-allocated) or NULL after reporting errors.
const char *driver_build(Arena *a, const char *src_path, const char *out_path, bool tests);
// Runs an executable and returns its exit status.
int driver_run(const char *exe_path);

#endif
