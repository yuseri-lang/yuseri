#include "driver.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cgen.h"
#include "check.h"
#include "lexer.h"
#include "parser.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// Directory of the running yuseri binary, used to find stage0/rt next to it.
static const char *exe_dir(Arena *a) {
  char buf[4096];
#ifdef __APPLE__
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return NULL;
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return NULL;
  buf[n] = 0;
#endif
  char real[4096];
  if (!realpath(buf, real)) return NULL;
  char *slash = strrchr(real, '/');
  if (!slash) return NULL;
  *slash = 0;
  return arena_cstr(a, str_cstr(real));
}

static bool dir_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// The runtime directory: $YUSERI_RT, or <exe dir>/../stage0/rt.
static const char *rt_dir(Arena *a) {
  const char *env = getenv("YUSERI_RT");
  if (env && dir_exists(env)) return env;
  const char *d = exe_dir(a);
  if (!d) return NULL;
  Buf b = {0};
  buf_printf(&b, "%s/../stage0/rt", d);
  const char *p = arena_cstr(a, str_from(b.p, b.len));
  buf_free(&b);
  return dir_exists(p) ? p : NULL;
}

static const char *base_name(Arena *a, const char *path) {
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  Str s = str_cstr(name);
  if (s.len > 3 && memcmp(s.p + s.len - 3, ".yu", 3) == 0) s.len -= 3;
  return arena_cstr(a, s);
}

const char *driver_build(Arena *a, const char *src_path, const char *out_path, bool tests) {
  Str src;
  if (!read_file(src_path, &src)) { fprintf(stderr, "yuseri: cannot read %s\n", src_path); return NULL; }
  int before = diag_count();
  diag_buffer(true);
  LexResult lx = lex(a, src_path, src);
  int after_lex = diag_count();
  Module *m = parse_module(a, src_path, lx);
  if (diag_count() == after_lex) check_module(a, m);
  diag_flush();
  diag_buffer(false);
  if (diag_count() != before) return NULL;

  const char *rt = rt_dir(a);
  if (!rt) { fprintf(stderr, "yuseri: runtime directory not found; set YUSERI_RT to stage0/rt\n"); return NULL; }

  Buf c = {0};
  cgen_module(a, m, tests, &c);

  const char *name = base_name(a, src_path);
  if (mkdir(".yuseri", 0755) != 0 && errno != EEXIST) { fprintf(stderr, "yuseri: cannot create .yuseri/\n"); return NULL; }
  Buf cpath = {0};
  buf_printf(&cpath, ".yuseri/%s%s.c", name, tests ? "_test" : "");
  FILE *f = fopen(cpath.p, "wb");
  if (!f) { fprintf(stderr, "yuseri: cannot write %s\n", cpath.p); return NULL; }
  fwrite(c.p, 1, (size_t)c.len, f);
  fclose(f);
  buf_free(&c);

  Buf exe = {0};
  if (out_path) buf_puts(&exe, out_path);
  else buf_printf(&exe, ".yuseri/%s%s", name, tests ? "_test" : "");
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "cc";
  Buf cmd = {0};
  buf_printf(&cmd, "%s -std=c11 -O1 -g -w -I'%s' -I'%s/../src' -o '%s' '%s' '%s'/*.c -lm", cc, rt, rt, exe.p, cpath.p, rt);
  int rc = system(cmd.p);
  buf_free(&cmd);
  buf_free(&cpath);
  if (rc != 0) { fprintf(stderr, "yuseri: the C compiler failed on the generated code (this is a compiler bug; see .yuseri/)\n"); buf_free(&exe); return NULL; }
  const char *result = arena_cstr(a, str_from(exe.p, exe.len));
  buf_free(&exe);
  return result;
}

int driver_run(const char *exe_path) {
  Buf cmd = {0};
  if (exe_path[0] == '/') buf_printf(&cmd, "'%s'", exe_path);
  else buf_printf(&cmd, "'./%s'", exe_path);
  int rc = system(cmd.p);
  buf_free(&cmd);
  if (rc == -1) return 127;
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}
