#!/usr/bin/env bash
#
# sql/test/run.sh - sql 包的单元测试入口。
#
#   bash test/run.sh
#   CT=/path/to/ct bash test/run.sh
#
# 做五件事：
#   1. 用 -Wall -Wextra 构建并运行 test/main.ct，断言零警告、零失败；
#   2. 断言断言条数等于固定值（与 README §4.7 的描述一致）；
#   3. 静态核对 test/main.ct 是否调用了全部公开方法名；
#   4. 静态核对变参分派（_Generic / SQL_ARGS / trait '...'）已彻底移除；
#   5. 构建并运行 demo，与 demo/expected.out 逐字节比对。
#
# 所有临时产物都在 /tmp 下，退出时清理。

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
CT="${CT:-/Users/samxx/.local/bin/ct}"
expected_assertions=129

if [ ! -x "$CT" ]; then
    if ! command -v "$CT" >/dev/null 2>&1; then
        echo "ct not found: $CT" >&2
        echo "set CT=/path/to/ct (0.20.3+)" >&2
        exit 1
    fi
fi

work="${TMPDIR:-/tmp}/ct-sql-test.$$"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT INT TERM

rm -rf "$work"
mkdir -p "$work" || exit 1

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s: %s\n' "$1" "$2"; }

echo "ct: $CT"
echo

echo "== build (test/main.ct, -Wall -Wextra) =="
(
    cd "$work" || exit 1
    "$CT" build -I "$root" --cflags "-Wall -Wextra" \
        "$root/test/main.ct" -o "$work/sql-test"
) >"$work/build.out" 2>"$work/build.err"
build_rc=$?

if [ "$build_rc" -ne 0 ]; then
    bad "build" "exit $build_rc"
    cat "$work/build.out" "$work/build.err"
    printf '\nsql: %d passed, %d failed\n' "$pass" "$fail"
    exit 1
fi
if [ -s "$work/build.err" ]; then
    bad "build warnings" "$(cat "$work/build.err")"
else
    ok "build (zero warnings)"
fi

echo
echo "== unit tests =="
"$work/sql-test" >"$work/run.out" 2>"$work/run.err"
run_rc=$?
cat "$work/run.out"
if [ -s "$work/run.err" ]; then
    cat "$work/run.err" >&2
fi

summary="$(grep '^RESULT:' "$work/run.out" | tail -1)"
if [ "$run_rc" -eq 0 ] && grep -q ', 0 failed' "$work/run.out"; then
    ok "assertions (${summary:-RESULT missing})"
else
    bad "assertions" "exit $run_rc; ${summary:-no RESULT line}"
    grep '^not ok ' "$work/run.out" || true
fi

actual="$(printf '%s' "$summary" | sed -n 's/^RESULT: \([0-9][0-9]*\) assertions.*/\1/p')"
if [ "$actual" = "$expected_assertions" ]; then
    ok "assertion count is $expected_assertions"
else
    bad "assertion count" "expected $expected_assertions, got ${actual:-none}"
fi

echo
echo "== method coverage (static: names called from test/main.ct) =="
names="select insert update delete where whereRaw bindInt bindDouble bindText bindNull row groupBy orderBy limit offset toSql sql paramCount paramKind paramInt paramDouble paramText status message free set setRaw allowAll"
total=0
missing=""
for m in $names; do
    total=$((total + 1))
    grep -q "$m(" "$root/test/main.ct" || missing="$missing $m"
done
if [ -z "$missing" ]; then
    ok "$total/$total public method names exercised"
else
    bad "method coverage" "never called:$missing"
fi

echo
echo "== variadic dispatch removed (static) =="
leak="$(grep -nE 'SQL_ARGS|SQL_NARG|SQL_VAL|_Generic' \
    "$root/sql/Sql.shim.h" "$root/sql/Sql.shim.c" "$root/sql/Sql.m.ct" 2>/dev/null)"
sig="$(grep -nE '^ *struct sql_[A-Za-z]+ \*[a-zA-Z]+\(.*\.\.\.' "$root/sql/Sql.m.ct" 2>/dev/null)"
if [ -z "$leak" ] && [ -z "$sig" ]; then
    ok "no variadic dispatch left in header/shim/mapping"
else
    bad "variadic dispatch" "still present: $leak $sig"
fi

echo
echo "== demo (output compared with demo/expected.out) =="
(
    cd "$work" || exit 1
    "$CT" build -I "$root" --cflags "-Wall -Wextra" \
        "$root/demo/main.ct" -o "$work/sql-demo"
) >"$work/demo-build.out" 2>"$work/demo-build.err"
demo_rc=$?
if [ "$demo_rc" -ne 0 ] || [ -s "$work/demo-build.err" ]; then
    bad "demo build" "exit $demo_rc; $(cat "$work/demo-build.err")"
    cat "$work/demo-build.out"
else
    "$work/sql-demo" >"$work/demo.out" 2>"$work/demo.err"
    demo_rc=$?
    if [ "$demo_rc" -ne 0 ]; then
        bad "demo run" "exit $demo_rc; $(cat "$work/demo.err")"
    elif diff -u "$root/demo/expected.out" "$work/demo.out" >"$work/demo.diff"; then
        ok "demo output matches expected.out"
    else
        bad "demo output" "diff vs demo/expected.out"
        cat "$work/demo.diff"
    fi
fi

echo
printf 'sql: %d passed, %d failed\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
