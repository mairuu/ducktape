#!/bin/sh
# run .dt test files against the compiler.
#   tests/pass/*.dt      must exit 0 with empty stderr
#   tests/warn/*.dt      must exit 0 with NON-empty stderr containing the
#                        `#! expect: <substring>` from its first line — the one
#                        cell of the (exit code x stderr) matrix the other
#                        buckets leave out, and the only one a warning fits
#   tests/fail/*.dt      must exit non-zero; if the first line is
#                        `#! expect: <substring>` stderr must contain it
#   tests/run/*.dt       executed with --run; must exit 0 with empty stderr and
#                        stdout equal to the `#> ` comment lines in the file
#   tests/fail_run/*.dt  like tests/fail, but invoked with --run — for things
#                        that type-check yet the VM rejects
#
# every tests/run program is additionally emitted as a bytecode image and
# re-run from it, so the suite doubles as the serialization round-trip suite.
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

check_warn() {
    f=$1
    err=$("$BIN" "$f" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -ne 0 ]; then
        fail=$((fail + 1))
        echo "FAIL (a warning must not fail the build): $f (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
        return
    fi
    if [ -z "$err" ]; then
        fail=$((fail + 1))
        echo "FAIL (expected a warning, got silence): $f"
        return
    fi
    expect=$(head -n1 "$f" | sed -n 's/^#! expect: //p')
    if [ -n "$expect" ] && ! printf '%s' "$err" | grep -qF "$expect"; then
        fail=$((fail + 1))
        echo "FAIL (wrong warning): $f"
        echo "    expected substring: $expect"
        printf '%s\n' "$err" | sed 's/^/    /'
        return
    fi
    pass=$((pass + 1))
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

for f in "$ROOT"/tests/warn/*.dt "$ROOT"/tests/warn/*/main.dt; do
    [ -e "$f" ] || continue
    check_warn "$f"
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
    check_image "$f" "$expected"
}

# every runnable program is also serialized, reloaded and re-run: the whole
# tests/run suite doubles as the round-trip suite, so a construct the image
# format forgets shows up as a diff rather than as a missing dedicated test.
check_image() {
    f=$1
    expected=$2
    img=$(mktemp)
    out_tmp=$(mktemp)

    err=$("$BIN" --emit-bc "$img" "$f" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -ne 0 ] || [ -n "$err" ]; then
        fail=$((fail + 1))
        echo "FAIL (emit failed): $f (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
        rm -f "$img" "$out_tmp"
        return
    fi

    err=$("$BIN" --run "$img" 2>&1 >"$out_tmp")
    code=$?
    actual=$(cat "$out_tmp")
    rm -f "$img" "$out_tmp"

    if [ "$code" -ne 0 ] || [ -n "$err" ]; then
        fail=$((fail + 1))
        echo "FAIL (image run failed): $f (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
        return
    fi
    if [ "$actual" != "$expected" ]; then
        fail=$((fail + 1))
        echo "FAIL (image output differs): $f"
        echo "    expected: $(printf '%s' "$expected" | tr '\n' '|')"
        echo "    actual:   $(printf '%s' "$actual" | tr '\n' '|')"
        return
    fi
    pass=$((pass + 1))
}

# a malformed image must be rejected, never executed.
check_bad_image() {  # $1 = description, $2 = file holding the image
    desc=$1
    img=$2
    err=$("$BIN" --run "$img" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -eq 0 ]; then
        fail=$((fail + 1))
        echo "FAIL (bad image accepted): $desc"
    else
        pass=$((pass + 1))
    fi
    rm -f "$img"
}

for f in "$ROOT"/tests/run/*.dt "$ROOT"/tests/run/*/main.dt; do
    [ -e "$f" ] || continue
    check_run "$f"
done

bad=$(mktemp)
printf 'DTBC\002\000\000\000' > "$bad"
check_bad_image "wrong version" "$bad"

bad=$(mktemp)
seed=$ROOT/tests/run/arithmetic.dt
if [ -e "$seed" ] && "$BIN" --emit-bc "$bad" "$seed" >/dev/null 2>&1; then
    dd if="$bad" of="$bad.cut" bs=1 count=32 2>/dev/null
    mv "$bad.cut" "$bad"
    check_bad_image "truncated image" "$bad"
else
    rm -f "$bad"
fi

# a native is written by name and re-bound at load, so an image naming one this
# build does not provide must be refused there rather than at the first call.
bad=$(mktemp)
seed=$ROOT/tests/run/native.dt
if [ -e "$seed" ] && "$BIN" --emit-bc "$bad" "$seed" >/dev/null 2>&1; then
    # same length, so every offset in the image stays valid
    sed 's/io_print/io_prinX/' < "$bad" > "$bad.mangled"
    mv "$bad.mangled" "$bad"
    check_bad_image "image naming an unknown native" "$bad"
else
    rm -f "$bad"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
