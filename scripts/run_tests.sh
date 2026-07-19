#!/bin/sh
# run .dt test files against the compiler.
#   tests/pass/*.dt  must exit 0 with empty stderr
#   tests/fail/*.dt  must exit non-zero; if the first line is
#                    `#! expect: <substring>` stderr must contain it
set -u

BIN=${1:-build/ducktape}
ROOT=$(dirname "$0")/..
pass=0
fail=0

check_pass() {
    f=$1
    err=$("$BIN" "$f" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -eq 0 ] && [ -z "$err" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL (expected pass): $f (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
    fi
}

check_fail() {
    f=$1
    err=$("$BIN" "$f" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -eq 0 ]; then
        fail=$((fail + 1))
        echo "FAIL (expected error): $f (exit 0)"
        return
    fi
    expect=$(head -n1 "$f" | sed -n 's/^#! expect: //p')
    if [ -n "$expect" ] && ! printf '%s' "$err" | grep -qF "$expect"; then
        fail=$((fail + 1))
        echo "FAIL (wrong error): $f"
        echo "    expected substring: $expect"
        printf '%s\n' "$err" | sed 's/^/    /'
        return
    fi
    pass=$((pass + 1))
}

for f in "$ROOT"/tests/pass/*.dt; do
    [ -e "$f" ] || continue
    check_pass "$f"
done

for f in "$ROOT"/tests/fail/*.dt; do
    [ -e "$f" ] || continue
    check_fail "$f"
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
