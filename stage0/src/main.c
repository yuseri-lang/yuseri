// Yuseri Stage 0: command line
#include "fmt.h"
#include "lexer.h"
#include "parser.h"

static int usage(void) {
  fprintf(stderr,
          "usage: yuseri fmt [--write] <file.yu>...   print (or rewrite) in canonical layout\n"
          "       yuseri check <file.yu>...           report E-FMT-01 when a file is not formatted\n"
          "       yuseri parse <file.yu>...           parse only and report errors\n");
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

static int cmd_fmt(int argc, char **argv) {
  bool write = false;
  int start = 0;
  if (argc > 0 && strcmp(argv[0], "--write") == 0) { write = true; start = 1; }
  if (start >= argc) return usage();
  int rc = 0;
  for (int i = start; i < argc; i++) {
    Buf out = {0};
    Str orig;
    if (!format_file(argv[i], &out, &orig)) { rc = 1; buf_free(&out); continue; }
    if (write) {
      if (!(orig.len == out.len && memcmp(orig.p, out.p, (size_t)out.len) == 0)) {
        FILE *fp = fopen(argv[i], "wb");
        if (!fp) { fprintf(stderr, "yuseri: cannot write %s\n", argv[i]); rc = 1; }
        else { fwrite(out.p, 1, (size_t)out.len, fp); fclose(fp); }
      }
    } else {
      fwrite(out.p, 1, (size_t)out.len, stdout);
    }
    free((char *)orig.p);
    buf_free(&out);
  }
  return rc;
}

static int cmd_check(int argc, char **argv) {
  if (argc == 0) return usage();
  int rc = 0;
  for (int i = 0; i < argc; i++) {
    Buf out = {0};
    Str orig;
    if (!format_file(argv[i], &out, &orig)) { rc = 1; buf_free(&out); continue; }
    if (!(orig.len == out.len && memcmp(orig.p, out.p, (size_t)out.len) == 0)) {
      diag_error("E-FMT-01", argv[i], 1, "file is not formatted");
      rc = 1;
    }
    free((char *)orig.p);
    buf_free(&out);
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

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  if (strcmp(argv[1], "fmt") == 0) return cmd_fmt(argc - 2, argv + 2);
  if (strcmp(argv[1], "check") == 0) return cmd_check(argc - 2, argv + 2);
  if (strcmp(argv[1], "parse") == 0) return cmd_parse(argc - 2, argv + 2);
  return usage();
}
