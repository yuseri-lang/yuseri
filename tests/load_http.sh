#!/bin/sh
# Load test (layer 2 L3): N workers, a Store updated by parallel requests, and the deadline.
# Not part of `make test` because it binds a port and needs curl. Usage: sh tests/load_http.sh
set -u
export YUSERI_RT=stage0/rt
build/yuseri build tests/run/load.yu -o .yuseri/load_server || exit 1
.yuseri/load_server 2>.yuseri/load_server.log &
pid=$!
started=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  if curl -s -o /dev/null http://127.0.0.1:8080/count; then started=1; break; fi
  perl -e 'select(undef, undef, undef, 0.2)'
done
if [ "$started" != 1 ]; then echo "FAIL    server did not start"; kill $pid 2>/dev/null; cat .yuseri/load_server.log; exit 1; fi
fail=0
head -1 .yuseri/load_server.log

# 1. parallel updates of one Store: every hit is counted exactly once
n=400
seq 1 $n | xargs -P 32 -I{} curl -s -o /dev/null -X POST http://127.0.0.1:8080/hit
count=$(curl -s http://127.0.0.1:8080/count)
if [ "$count" = "$n" ]; then echo "ok      $n parallel updates counted exactly"; else echo "FAIL    count: got $count, want $n"; fail=1; fi

# 2. a handler that never returns is stopped by the 500 ms deadline and answered with 500
start=$(date +%s)
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://127.0.0.1:8080/slow)
elapsed=$(( $(date +%s) - start ))
if [ "$status" = 500 ] && [ "$elapsed" -le 2 ]; then echo "ok      deadline answers 500 within ${elapsed}s"; else echo "FAIL    deadline: status $status after ${elapsed}s"; fail=1; fi
if grep -q '^req-[0-9]* R-TIME-01 ' .yuseri/load_server.log; then echo "ok      R-TIME-01 is logged with the request id"; else echo "FAIL    no 'req-N R-TIME-01' line in the log"; fail=1; fi

# 3. the worker that hit the deadline keeps serving
status=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/count)
if [ "$status" = 200 ]; then echo "ok      server keeps serving after a deadline"; else echo "FAIL    after deadline: status $status"; fail=1; fi

# 4. a slow request does not block the others: 8 slow ones in parallel, then a fast one answers
slow_pids=""
for i in 1 2 3 4 5 6 7 8; do
  curl -s -o /dev/null --max-time 5 http://127.0.0.1:8080/slow &
  slow_pids="$slow_pids $!"
done
perl -e 'select(undef, undef, undef, 0.1)'
start=$(date +%s)
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://127.0.0.1:8080/count)
elapsed=$(( $(date +%s) - start ))
wait $slow_pids
if [ "$status" = 200 ] && [ "$elapsed" -le 2 ]; then echo "ok      a fast request is answered while slow ones run (${elapsed}s)"; else echo "FAIL    fast request: status $status after ${elapsed}s"; fail=1; fi

# 5. spawned tasks run after the response: /later answers the old count, /count shows the new one
before=$(curl -s http://127.0.0.1:8080/count)
answered=$(curl -s -X POST http://127.0.0.1:8080/later)
after=$(curl -s http://127.0.0.1:8080/count)
if [ "$answered" = "$before" ] && [ "$after" = "$(( before + 1 ))" ]; then echo "ok      a spawned task runs after the response ($before -> $after)"; else echo "FAIL    spawn: before $before, answered $answered, after $after"; fail=1; fi
status=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:8080/later-fail)
if [ "$status" = 202 ] && grep -q '^req-[0-9]* R-TASK-01 ' .yuseri/load_server.log; then echo "ok      a failing task is logged as R-TASK-01 with the request id"; else echo "FAIL    task failure: status $status"; fail=1; fi

kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "server log (R-TIME-01 / R-TASK-01 lines):"
grep 'R-TIME-01\|R-TASK-01' .yuseri/load_server.log | head -4
exit $fail
