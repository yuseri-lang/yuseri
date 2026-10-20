#!/bin/sh
# Manual smoke test: builds the eval API server, serves it on port 8080, talks HTTP to it.
# Not part of `make test` because it binds a port. Usage: sh tests/smoke_http.sh
set -u
export YUSERI_RT=stage0/rt
build/yuseri build tests/run/eval_api.yu -o .yuseri/smoke_server || exit 1
.yuseri/smoke_server 2>.yuseri/smoke_server.log &
pid=$!
started=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  if curl -s -o /dev/null http://127.0.0.1:8080/health; then started=1; break; fi
  perl -e 'select(undef, undef, undef, 0.2)'
done
if [ "$started" != 1 ]; then echo "FAIL    server did not start"; kill $pid 2>/dev/null; cat .yuseri/smoke_server.log; exit 1; fi
fail=0
# check <name> <want-status> <want-body> <curl args...>
check() {
  name=$1; want_status=$2; want_body=$3; shift 3
  status=$(curl -s -o .yuseri/smoke_body -w '%{http_code}' "$@")
  body=$(cat .yuseri/smoke_body)
  if [ "$status" = "$want_status" ] && [ "$body" = "$want_body" ]; then echo "ok      $name"
  else echo "FAIL    $name: got $status $body"; fail=1; fi
}
check "GET /health" 200 "ok" http://127.0.0.1:8080/health
check "POST /users" 201 '{"id":1,"name":"Ann","age":30,"email":"ann@example.com"}' -X POST -d '{"name":"Ann","age":30,"email":"ann@example.com"}' http://127.0.0.1:8080/users
check "POST /users invalid" 400 '{"error":"email must contain @"}' -X POST -d '{"name":"Ann","age":30,"email":"ann"}' http://127.0.0.1:8080/users
check "POST /users bad json" 400 '{"error":"bad json"}' -X POST -d '{x' http://127.0.0.1:8080/users
check "GET /users/7" 200 '{"id":7,"name":"user7","age":30,"email":"user7@example.com"}' http://127.0.0.1:8080/users/7
check "GET /users/abc" 404 '{"error":"not found"}' http://127.0.0.1:8080/users/abc
check "GET /nope" 404 "not found" http://127.0.0.1:8080/nope
ctype=$(curl -s -D - -o /dev/null http://127.0.0.1:8080/users/1 | grep -i '^content-type:' | tr -d '\r')
if [ "$ctype" = "content-type: application/json" ]; then echo "ok      content-type is json"; else echo "FAIL    content-type: got $ctype"; fail=1; fi
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "server log:"
cat .yuseri/smoke_server.log
exit $fail
