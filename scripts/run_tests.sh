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
# a `#! flags: …` line in any of those hands the compiler extra arguments; that
# is how the `-W` lint levels, which no source can set, get tested at all
#
# every tests/run program is additionally emitted as a bytecode image and
# re-run from it, so the suite doubles as the serialization round-trip suite.
#
# multi-file tests live in a subdirectory of any of the above, entry point
# `main.dt`, imported modules alongside it. The flat globs are non-recursive,
# so those siblings are never picked up as tests in their own right.
#
# every std module is linted under the same rule as tests/pass, via
# `--std-module std::<path>`: the module path says which module it is, its
# source is read from disk, and a root always has a warning audience — so std's
# own warnings fail the suite instead of hiding behind the silence every
# *program* gets to keep. The list is derived from the tree, so a module that
# stops being declared stops being linted *and* stops existing.
set -u

BIN=${1:-build/ducktape}
# resolved against the caller's cwd, because the std lint runs from $ROOT.
case $BIN in /*) ;; *) BIN=$(pwd)/$BIN ;; esac
ROOT=$(dirname "$0")/..
pass=0
fail=0

# a test may name the compiler flags it needs, on any line: `#! flags: -Werror`.
# The lint levels are what this exists for — a level set from the command line
# is the one policy the source cannot state, so it has to come from here.
# Deliberately unquoted at the call sites, to split into separate arguments.
test_flags() { sed -n 's/^#! flags: //p' "$1" | head -n1; }

check_pass() {
    f=$1
    err=$("$BIN" $(test_flags "$f") "$f" 2>&1 >/dev/null)
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
    err=$("$BIN" $(test_flags "$f") "$f" 2>&1 >/dev/null)
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
    err=$("$BIN" "$@" $(test_flags "$f") "$f" 2>&1 >/dev/null)
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

check_std_module() {   # $1 = module path, e.g. std::collections::hashmap
    err=$(cd "$ROOT" && "$BIN" --std-module "$1" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -eq 0 ] && [ -z "$err" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL (expected a clean lint): $1 (exit $code)"
        [ -n "$err" ] && printf '%s\n' "$err" | sed 's/^/    /'
    fi
}

# derived from the files, not written down: a std module that gains a file but
# no `pub mod` fails here as "there is no standard library module".
for f in "$ROOT"/std/*.dt "$ROOT"/std/*/*.dt; do
    [ -e "$f" ] || continue
    rel=${f#"$ROOT"/std/}
    case $rel in
        lib.dt) check_std_module std ;;   # the root, which `std` alone names
        *) check_std_module "std::$(printf '%s' "${rel%.dt}" | sed 's|/|::|g')" ;;
    esac
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

# a malformed image must be rejected, never executed. An expected substring is
# optional and is what a mangling that could have hit more than one check has
# to give: without it, being refused for the wrong reason still counts.
check_bad_image() {  # $1 = description, $2 = image file, $3 = expected (opt)
    desc=$1
    img=$2
    expect=${3:-}
    err=$("$BIN" --run "$img" 2>&1 >/dev/null)
    code=$?
    if [ "$code" -eq 0 ]; then
        fail=$((fail + 1))
        echo "FAIL (bad image accepted): $desc"
    elif [ -n "$expect" ] && ! printf '%s' "$err" | grep -qF "$expect"; then
        fail=$((fail + 1))
        echo "FAIL (bad image refused for the wrong reason): $desc"
        echo "    expected substring: $expect"
        printf '%s\n' "$err" | sed 's/^/    /'
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

# the second untrusted intake: every String an image can produce is interned
# from its string table, so the table is checked at load the way a source file
# is checked at read. The same trick as above — one byte of a long, unique name
# replaced by 0xFF, which begins no sequence — and the table is read before
# anything is bound, so this is refused there rather than as an unknown native.
bad=$(mktemp)
seed=$ROOT/tests/run/native.dt
if [ -e "$seed" ] && "$BIN" --emit-bc "$bad" "$seed" >/dev/null 2>&1; then
    LC_ALL=C sed "s/io_print/io_prin$(printf '\377')/" < "$bad" > "$bad.mangled"
    mv "$bad.mangled" "$bad"
    check_bad_image "image string table that is not UTF-8" "$bad" \
        "is not valid UTF-8"
else
    rm -f "$bad"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
