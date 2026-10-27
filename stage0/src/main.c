// Yuseri Stage 0: command line
#include "check.h"
#include "driver.h"
#include "fmt.h"
#include "lexer.h"
#include "parser.h"

static int usage(void) {
  fprintf(stderr,
          "usage: yuseri fmt [--write|--check] <file.yu>...   print, rewrite, or check the canonical layout\n"
          "       yuseri check <file.yu>...           type-check and report every error\n"
          "       yuseri parse <file.yu>...           parse only and report errors\n"
          "       yuseri build <file.yu> [-o <exe>]    compile to a native executable\n"
          "       yuseri run <file.yu>                 build and run main\n"
          "       yuseri test <file.yu>                build and run the test blocks\n");
  return 2;
}

// Formats one file. Returns false when it could not be read or parsed.
static bool format_file(const char *path, Buf *out, Str *original) {
  Str src;
  if (!read_file(path, &src)) {
    fprintf(stderr, "yuseri: cannot read %s\n", path);
    return false;
  }
  Arena a = {0};
  int before = diag_count();
  LexResult lx = lex(&a, path, src);
  Module *m = parse_module(&a, path, lx);
  bool ok = diag_count() == before;
  if (ok) fmt_module(m, out);
  arena_free(&a);
  if (original) *original = src;
  else free((char *)src.p);
  return ok;
}

// True when the source already has the canonical layout.
static bool is_formatted(Str src, const Buf *out) {
  return src.len == out->len && memcmp(src.p, out->p, (size_t)out->len) == 0;
}

// 1-based number of the first line where the source differs from the canonical layout.
static int first_diff_line(Str src, const Buf *out) {
  int line = 1, k = 0;
  while (k < src.len && k < out->len && src.p[k] == out->p[k]) { if (src.p[k] == '\n') line++; k++; }
  return line;
}

// `fmt` prints the canonical layout; `--write` rewrites the file in place;
// `--check` prints nothing and only reports files that are not formatted (exit 1).
static int cmd_fmt(int argc, char **argv) {
  bool write = false, check = false;
  int start = 0;
  if (argc > 0 && strcmp(argv[0], "--write") == 0) { write = true; start = 1; }
  else if (argc > 0 && strcmp(argv[0], "--check") == 0) { check = true; start = 1; }
  if (start >= argc) return usage();
  int rc = 0;
  for (int i = start; i < argc; i++) {
    Buf out = {0};
    Str orig;
    if (!format_file(argv[i], &out, &orig)) { rc = 1; buf_free(&out); continue; }
    if (write) {
      if (!is_formatted(orig, &out)) {
        FILE *fp = fopen(argv[i], "wb");
        if (!fp) { fprintf(stderr, "yuseri: cannot write %s\n", argv[i]); rc = 1; }
        else { fwrite(out.p, 1, (size_t)out.len, fp); fclose(fp); }
      }
    } else if (check) {
      if (!is_formatted(orig, &out)) {
        fprintf(stderr, "%s:%d is not formatted\n", argv[i], first_diff_line(orig, &out));
        rc = 1;
      }
    } else {
      fwrite(out.p, 1, (size_t)out.len, stdout);
    }
    free((char *)orig.p);
    buf_free(&out);
  }
  return rc;
}

// Lexes, parses and type-checks one file; errors are printed sorted by line.
// Layout is not checked here: that is `yuseri fmt --check`.
static int cmd_check(int argc, char **argv) {
  if (argc == 0) return usage();
  int rc = 0;
  for (int i = 0; i < argc; i++) {
    Str src;
    if (!read_file(argv[i], &src)) { fprintf(stderr, "yuseri: cannot read %s\n", argv[i]); rc = 1; continue; }
    Arena a = {0};
    diag_buffer(true);
    int before = diag_count();
    LexResult lx = lex(&a, argv[i], src);
    int after_lex = diag_count();
    Module *m = parse_module(&a, argv[i], lx);
    int after_parse = diag_count();
    bool syntax_ok = after_parse == after_lex;
    if (syntax_ok) check_module(&a, m);
    diag_flush();
    diag_buffer(false);
    if (diag_count() != before) rc = 1;
    arena_free(&a);
    free((char *)src.p);
  }
  return rc;
}

static int cmd_parse(int argc, char **argv) {
  if (argc == 0) return usage();
  int rc = 0;
  for (int i = 0; i < argc; i++) {
    Str src;
    if (!read_file(argv[i], &src)) { fprintf(stderr, "yuseri: cannot read %s\n", argv[i]); rc = 1; continue; }
    Arena a = {0};
    int before = diag_count();
    LexResult lx = lex(&a, argv[i], src);
    parse_module(&a, argv[i], lx);
    if (diag_count() != before) rc = 1;
    arena_free(&a);
    free((char *)src.p);
  }
  return rc;
}

static int cmd_build(int argc, char **argv, int mode) {  // mode: 0 build, 1 run, 2 test
  if (argc == 0) return usage();
  const char *out = NULL;
  if (argc >= 3 && strcmp(argv[1], "-o") == 0) out = argv[2];
  Arena a = {0};
  const char *exe = driver_build(&a, argv[0], out, mode == 2);
  int rc = exe ? 0 : 1;
  if (exe && mode != 0) rc = driver_run(exe);
  arena_free(&a);
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  if (strcmp(argv[1], "build") == 0) return cmd_build(argc - 2, argv + 2, 0);
  if (strcmp(argv[1], "run") == 0) return cmd_build(argc - 2, argv + 2, 1);
  if (strcmp(argv[1], "test") == 0) return cmd_build(argc - 2, argv + 2, 2);
  if (strcmp(argv[1], "fmt") == 0) return cmd_fmt(argc - 2, argv + 2);
  if (strcmp(argv[1], "check") == 0) return cmd_check(argc - 2, argv + 2);
  if (strcmp(argv[1], "parse") == 0) return cmd_parse(argc - 2, argv + 2);
  return usage();
}
