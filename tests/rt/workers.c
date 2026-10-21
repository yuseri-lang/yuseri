// Runtime tests for layer 2 L3: Store serialization across threads, deadlines and nested
// updates. Built and run by tests/run_rt.sh with the address and undefined-behavior sanitizers.
#include <pthread.h>

#include "yu_rt.h"

static int failures = 0;

static void check(const char *name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) failures++;
}

// Exactly what the generated code does for `n.update(fn(v: Int) -> Int { v + 1 })`.
static void bump(yu_store *s) {
  yu_store_lock(s, "workers.c:bump");
  int64_t cur = *(int64_t *)s->value;
  int64_t nv = cur + 1;
  yu_arena *prev = yu_store_begin(s);
  int64_t cp = nv;
  yu_store_commit(s, &cp, prev);
  yu_store_unlock(s);
}

static int64_t current(yu_store *s) {
  yu_store_lock(s, "workers.c:current");
  int64_t v = *(int64_t *)s->value;
  yu_store_unlock(s);
  return v;
}

#define THREADS 8
#define ROUNDS 20000

static void *hammer(void *arg) {
  yu_arena *mine = yu_arena_new();  // a worker allocates on its own arena
  yu_arena_use(mine);
  for (int i = 0; i < ROUNDS; i++) bump(arg);
  yu_arena_use(NULL);  // released on exit, so LeakSanitizer (on by default on Linux) stays quiet
  yu_arena_free(mine);
  return NULL;
}

static yu_arena *current_arena(void) {
  yu_arena *cur = yu_arena_use(NULL);
  yu_arena_use(cur);
  return cur;
}

int main(void) {
  // 1. N threads, ROUNDS updates each: the count is exact (no lost update, no torn arena)
  yu_store *s = yu_store_new(sizeof(int64_t));
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, hammer, s);
  for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
  check("updates from 8 threads are serialized", current(s) == THREADS * ROUNDS);

  // 2. a deadline in the past stops the next allocations with R-TIME-01, caught like a panic
  bool caught = false;
  yu_deadline_set(yu_now_ms() - 1, 10);
  if (yu_catch_begin()) {
    for (int i = 0; i < 100000; i++) yu_alloc(16);
    yu_catch_end();
  } else caught = true;
  check("a passed deadline stops allocation", caught);
  check("the failure clears the deadline", yu_deadline_remaining_ms() == 0);

  // 3. ...and a loop that never allocates, through yu_tick at each iteration
  caught = false;
  yu_deadline_set(yu_now_ms() - 1, 10);
  if (yu_catch_begin()) {
    for (int i = 0; i < 100000; i++) yu_tick();
    yu_catch_end();
  } else caught = true;
  check("a passed deadline stops a loop", caught);
  yu_deadline_set(0, 0);

  // 4. update inside an update of the same store is R-STORE-02, and the failure releases the lock
  caught = false;
  if (yu_catch_begin()) {
    yu_store_lock(s, "workers.c:outer");
    yu_store_lock(s, "workers.c:inner");
    yu_catch_end();
  } else caught = true;
  check("update inside update fails with R-STORE-02", caught);
  bump(s);  // would deadlock if the failure had left the lock held
  check("the failure released the lock", current(s) == THREADS * ROUNDS + 1);

  // 5. a failure between begin and commit frees the pending arena and restores the current one
  yu_arena *before = current_arena();
  caught = false;
  if (yu_catch_begin()) {
    yu_store_lock(s, "workers.c:mid");
    yu_store_begin(s);
    yu_fail("R-PANIC-01", "workers.c", "panic: boom");
    yu_catch_end();
  } else caught = true;
  check("a failure mid-update restores the arena", caught && current_arena() == before);
  check("the value is unchanged", current(s) == THREADS * ROUNDS + 1);
  bump(s);
  check("the store works after a mid-update failure", current(s) == THREADS * ROUNDS + 2);
  return failures ? 1 : 0;
}
