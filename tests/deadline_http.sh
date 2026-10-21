#!/bin/sh
# Deadline test: where R-TIME-01 points. The position must be the loop that is running, never a
# loop that already ended (in the handler or in a callee that returned from inside its loop), and
# `std.http` when no loop is running. Not part of `make test` because it binds a port and needs
# curl. Usage: sh tests/deadline_http.sh
set -u
export YUSERI_RT=stage0/rt
src=tests/run/deadline.yu
log=.yuseri/deadline_server.log
build/yuseri build $src -o .yuseri/deadline_server || exit 1
.yuseri/deadline_server 2>$log &
pid=$!
started=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  if curl -s -o /dev/null http://127.0.0.1:8080/nope; then started=1; break; fi
  perl -e 'select(undef, undef, undef, 0.2)'
done
if [ "$started" != 1 ]; then echo "FAIL    server did not start"; kill $pid 2>/dev/null; cat $log; exit 1; fi
fail=0
# the lines of the loops, read from the source so the checks do not go stale when it is edited
in_loop=$(grep -n 'while true' $src | cut -d: -f1)
ended_loop=$(grep -n 'for i in 0..3' $src | cut -d: -f1)
callee_loop=$(grep -n 'for x in xs' $src | cut -d: -f1)

# check <route> <want-loc>: the request is answered with 500 and its R-TIME-01 line names <want-loc>
check() {
  route=$1; want=$2
  status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:8080$route")
  line=$(grep "R-TIME-01 .* request GET $route " $log | tail -1)
  case "$line" in
    *" R-TIME-01 $want request GET $route "*)
      if [ "$status" = 500 ]; then echo "ok      $route -> R-TIME-01 $want"; return; fi ;;
  esac
  echo "FAIL    $route: status $status, line: ${line:-<none>}"
  fail=1
}
check /in-loop "$src:$in_loop"
check /after-loop std.http
check /after-call std.http
if grep -q -e "R-TIME-01 $src:$ended_loop " -e "R-TIME-01 $src:$callee_loop " $log; then
  echo "FAIL    a loop that had ended was reported"
  fail=1
fi
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "server log:"
cat $log
exit $fail
