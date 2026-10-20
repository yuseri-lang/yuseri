// Runtime tests for the real capabilities of layer 2 L4: Fs on the disk, Random from the OS,
// and the task queue under http.serve. Built and run by tests/run_rt.sh with sanitizers.
#include <unistd.h>

#include "yu_rt.h"

static int failures = 0;

static void check(const char *name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) failures++;
}

static int g_ran = 0;
static yu_unit task_ok(void *env) { (void)env; g_ran++; return YU_UNIT; }
static yu_unit task_bad(void *env) { (void)env; yu_fail("R-PANIC-01", "caps.c", "panic: task boom"); }

int main(void) {
  yu_system sys = yu_system_new();

  // Fs: write, read back, overwrite, missing file, unwritable path
  char path[64];
  snprintf(path, sizeof path, "/tmp/yuseri-caps-%d.txt", (int)getpid());
  yu_str p = yu_str_lit(path, (int64_t)strlen(path)), out = {0}, msg = {0};
  check("write a file", yu_fs_write(sys.f_fs, p, yu_str_lit("hello\n", 6), &msg) == 0);
  check("read it back", yu_fs_read(sys.f_fs, p, &out, &msg) == 0 && yu_str_eq(out, yu_str_lit("hello\n", 6)));
  check("overwrite", yu_fs_write(sys.f_fs, p, yu_str_lit("", 0), &msg) == 0 && yu_fs_read(sys.f_fs, p, &out, &msg) == 0 && out.len == 0);
  unlink(path);
  check("a missing file is NotFound", yu_fs_read(sys.f_fs, p, &out, &msg) == 1);
  check("a missing directory is NotFound on write", yu_fs_write(sys.f_fs, yu_str_lit("/nonexistent-dir/x", 18), yu_str_lit("x", 1), &msg) == 1);
  check("a directory is an Io error on read", yu_fs_read(sys.f_fs, yu_str_lit("/tmp", 4), &out, &msg) == 2 && msg.len > 0);

  // Random: in range, not constant, and seeded streams repeat
  bool in_range = true, varied = false;
  int64_t first = yu_random_int(sys.f_random, 1000, "caps.c");
  for (int i = 0; i < 10000; i++) {
    int64_t v = yu_random_int(sys.f_random, 1000, "caps.c");
    if (v < 0 || v >= 1000) in_range = false;
    if (v != first) varied = true;
  }
  check("random.int stays in range", in_range);
  check("random.int is not constant", varied);
  check("random.int(1) is 0", yu_random_int(sys.f_random, 1, "caps.c") == 0);
  yu_random *a = yu_random_seeded(7), *b = yu_random_seeded(7);
  bool same = true;
  for (int i = 0; i < 100; i++) if (yu_random_int(a, 1000000, "caps.c") != yu_random_int(b, 1000000, "caps.c")) same = false;
  check("seeded streams repeat", same);
  bool caught = false;
  if (yu_catch_begin()) { yu_random_int(a, 0, "caps.c"); yu_catch_end(); } else caught = true;
  check("max <= 0 is R-RAND-01", caught);

  // Tasks: queued while serving, run by yu_tasks_run; a failing task stops only itself
  yu_serving = true;
  yu_tasks_spawn(sys.f_tasks, task_ok, NULL, "caps.c:1");
  yu_tasks_spawn(sys.f_tasks, task_bad, NULL, "caps.c:2");
  yu_tasks_spawn(sys.f_tasks, task_ok, NULL, "caps.c:3");
  check("tasks wait for the response while serving", g_ran == 0);
  yu_tasks_run();
  check("all tasks ran; the failing one stopped only itself", g_ran == 2);
  yu_serving = false;
  yu_tasks_spawn(sys.f_tasks, task_ok, NULL, "caps.c:4");
  check("outside serving a task runs at once", g_ran == 3);
  yu_tasks *inl = yu_tasks_inline();
  yu_tasks_spawn(inl, task_ok, NULL, "caps.c:5");
  check("Tasks.inline runs at once", g_ran == 4);
  return failures ? 1 : 0;
}
