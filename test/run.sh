#!/usr/bin/env bash
#
# sql/test/run.sh - sql 包的单元测试入口。
#
#   bash test/run.sh
#   CT=/path/to/ct bash test/run.sh
#
# 做三件事：
#   1. 用 -std=c11 -Wall -Wextra 构建并运行 test/main.ct，断言零警告、零失败；
#   2. 编译 test/overlimit.ct（9 个绑定值），断言在宏分派处失败；
#   3. 静态核对 test/main.ct 是否调用了全部公开方法名。
#
# 所有临时产物都在 /tmp 下，退出时清理。

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
CT="${CT:-/tmp/ct-0.20.0-worktree/ct}"

if [ ! -x "$CT" ]; then
    if ! command -v "$CT" >/dev/null 2>&1; then
        echo "ct not found: $CT" >&2
        echo "set CT=/path/to/ct (0.20.0+)" >&2
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

echo "== build (test/main.ct, -std=c11 -Wall -Wextra) =="
(
    cd "$work" || exit 1
    "$CT" build -I "$root" --cflags "-std=c11 -Wall -Wextra" \
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

echo
echo "== negative fixture (9 values must not compile) =="
(
    cd "$work" || exit 1
    "$CT" build -I "$root" --cflags "-std=c11 -Wall -Wextra" \
        "$root/test/overlimit.ct" -o "$work/overlimit"
) >"$work/overlimit.log" 2>&1
over_rc=$?
if [ "$over_rc" -eq 0 ]; then
    bad "overlimit" "build unexpectedly succeeded"
elif grep -q 'SQL_ARGS_' "$work/overlimit.log"; then
    ok "9 values rejected at SQL_ARGS_ dispatch"
else
    bad "overlimit" "failed, but not at SQL_ARGS_ dispatch; see log"
    cat "$work/overlimit.log"
fi

echo
echo "== method coverage (static: names called from test/main.ct) =="
names="select insert update delete where whereRaw groupBy orderBy limit offset toSql sql paramCount paramKind paramInt paramDouble paramText status message free values set setRaw allowAll"
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
printf 'sql: %d passed, %d failed\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
