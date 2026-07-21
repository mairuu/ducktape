#!/bin/sh
# run .dt test files against the compiler.
#   tests/pass/*.dt      must exit 0 with empty stderr
#   tests/fail/*.dt      must exit non-zero; if the first line is
#                        `#! expect: <substring>` stderr must contain it
#   tests/run/*.dt       executed with --run; must exit 0 with empty stderr and
#                        stdout equal to the `#> ` comment lines in the file
#   tests/fail_run/*.dt  like tests/fail, but invoked with --run — for things
#                        that type-check yet the VM rejects
#
# multi-file tests live in a subdirectory of any of the above, entry point
# `main.dt`, imported modules alongside it. The flat globs are non-recursive,
# so those siblings are never picked up as tests in their own right.
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

check_fail() {          # $1 = file, $2... = extra flags for the compiler
    f=$1
    shift
    err=$("$BIN" "$@" "$f" 2>&1 >/dev/null)
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

for f in "$ROOT"/tests/pass/*.dt "$ROOT"/tests/pass/*/main.dt; do
    [ -e "$f" ] || continue
    check_pass "$f"
done

for f in "$ROOT"/tests/fail/*.dt "$ROOT"/tests/fail/*/main.dt; do
    [ -e "$f" ] || continue
    check_fail "$f"
done

for f in "$ROOT"/tests/fail_run/*.dt "$ROOT"/tests/fail_run/*/main.dt; do
    [ -e "$f" ] || continue
    check_fail "$f" --run
done

check_run() {
    f=$1
    out_tmp=$(mktemp)
    err=$("$BIN" --run "$f" 2>&1 >"$out_tmp")
    code=$?
    expected=$(sed -n 's/^#> \{0,1\}//p' "$f")
    actual=$(cat "$out_tmp")
    rm -f "$out_tmp"

    if [ "$code" -ne 0 ] || [ -n "$err" ]; then
        fail=$((fail + 1))
        echo "FAIL (expected clean run): $f (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
        return
    fi
    if [ "$actual" != "$expected" ]; then
        fail=$((fail + 1))
        echo "FAIL (wrong output): $f"
        echo "    expected: $(printf '%s' "$expected" | tr '\n' '|')"
        echo "    actual:   $(printf '%s' "$actual" | tr '\n' '|')"
        return
    fi
    pass=$((pass + 1))
}

for f in "$ROOT"/tests/run/*.dt; do
    [ -e "$f" ] || continue
    check_run "$f"
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
