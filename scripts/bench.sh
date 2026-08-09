#!/bin/sh
# time the .dt programs in bench/ and print the tables runtime.md quotes.
#
#   sh scripts/bench.sh                          one binary
#   sh scripts/bench.sh --against build/before   two binaries, side by side
#   sh scripts/bench.sh --runs 9 --table iter    more repeats, one table
#
# One directory under bench/ is one table, and the files in it are its rows in
# name order — hence the numeric prefixes. Each declares itself in its header,
# the way a test does:
#
#   #! bench: <the label the row gets>
#   #! n: <elements>       what the elapsed time is divided by; one per table
#   #! baseline            exactly one file per table says this
#
# Every row is reported as ns/elem *over its table's baseline*, which is what
# cancels process startup, the image load and whatever setup the programs
# share. A table whose baseline does nothing therefore reads as absolute cost.
#
# WHAT THIS HARNESS IS FOR, and the rule milestone 110 paid for: a delta is
# only worth reading once the two things it is between have been shown to be
# the same measurement. So, in this order, before any delta is printed:
#
#   - each program is compiled to an image once and *that* is what is timed,
#     so no row carries its own compile;
#   - every program is run under every binary and the outputs must be
#     identical — a faster binary that computes something else is not faster;
#   - the baseline's wall time is printed, every time, per binary. It is the
#     one number on the page that can be compared against a previous run: if
#     it moved, the machine moved, and no delta here belongs in the same table
#     as one written on another day.
#
# Build with `make BUILD=release` first — a debug binary is ~10x slower, and
# the baseline row is where that shows.
set -u

ROOT=$(dirname "$0")/..
RUNS=7
ONLY=
BIN=
OTHER=
TAB=$(printf '\t')

usage() {
    echo "usage: $0 [--against <binary>] [--runs <n>] [--table <name>] [<binary>]" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
        --against) [ $# -ge 2 ] || usage; OTHER=$2; shift 2 ;;
        --runs)    [ $# -ge 2 ] || usage; RUNS=$2; shift 2 ;;
        --table)   [ $# -ge 2 ] || usage; ONLY=$2; shift 2 ;;
        -h|--help) usage ;;
        -*)        usage ;;
        *)         [ -z "$BIN" ] || usage; BIN=$1; shift ;;
    esac
done
BIN=${BIN:-$ROOT/build/ducktape}

for b in "$BIN" ${OTHER:+"$OTHER"}; do
    [ -x "$b" ] || { echo "not an executable: $b" >&2; exit 1; }
done

# nanosecond wall clock. `date +%N` is a GNU extension; without it every
# measurement below would silently round to the second.
now_ns() { date +%s%N; }
case $(now_ns) in
    *[!0-9]*|'') echo "this shell's date(1) has no %N; cannot time anything" >&2
                 exit 1 ;;
esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

header() { sed -n "s/^#! $1: //p" "$2" | head -n1; }

# ── the manifest: one line per row, baselines first within their table ───────
for f in "$ROOT"/bench/*/*.dt; do
    [ -e "$f" ] || continue
    table=$(basename "$(dirname "$f")")
    [ -z "$ONLY" ] || [ "$ONLY" = "$table" ] || continue
    label=$(header bench "$f")
    n=$(header n "$f")
    if [ -z "$label" ] || [ -z "$n" ]; then
        echo "$f: needs a #! bench: label and an #! n: count" >&2
        exit 1
    fi
    if grep -q '^#! baseline$' "$f"; then order=0; else order=1; fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$table" "$order" "$(basename "$f")" \
        "$n" "$f" "$label"
done | sort -t"$TAB" -k1,1 -k2,2 -k3,3 > "$TMP/manifest"

[ -s "$TMP/manifest" ] || { echo "no bench programs${ONLY:+ in $ONLY}" >&2; exit 1; }

# one baseline per table, one n per table — a row measured over a different
# count is not a row of the same table.
while IFS="$TAB" read -r table order name n f label; do
    base_n=$(sed -n "s/^$table$TAB//p" "$TMP/tables" 2>/dev/null | head -n1)
    if [ "$order" = 0 ]; then
        [ -z "$base_n" ] || { echo "bench/$table: more than one #! baseline" >&2; exit 1; }
        printf '%s\t%s\n' "$table" "$n" >> "$TMP/tables"
    elif [ -z "$base_n" ]; then
        echo "bench/$table: no #! baseline" >&2; exit 1
    elif [ "$base_n" != "$n" ]; then
        echo "$f: n=$n, but bench/$table's baseline says $base_n" >&2; exit 1
    fi
done < "$TMP/manifest"

# ── compile once per (binary, program), and check the binaries agree ─────────
img_of() { echo "$TMP/$1.$2.$(basename "$3" .dt).dtbc"; }
out_of() { echo "$TMP/$1.$2.$(basename "$3" .dt).out"; }

build_and_run() {  # $1 = tag, $2 = binary, $3 = table, $4 = source
    img=$(img_of "$1" "$3" "$4")
    if ! "$2" --emit-bc "$img" "$4" >/dev/null 2>"$TMP/err" </dev/null; then
        echo; echo "FAILED to compile $4 with $2:" >&2
        sed 's/^/    /' "$TMP/err" >&2
        exit 1
    fi
    if ! "$2" --run "$img" >"$(out_of "$1" "$3" "$4")" 2>"$TMP/err" </dev/null; then
        echo; echo "FAILED to run $4 with $2:" >&2
        sed 's/^/    /' "$TMP/err" >&2
        exit 1
    fi
}

printf 'compiling'
while IFS="$TAB" read -r table order name n f label; do
    printf '.'
    build_and_run a "$BIN" "$table" "$f"
    [ -n "$OTHER" ] || continue
    build_and_run b "$OTHER" "$table" "$f"
    if ! cmp -s "$(out_of a "$table" "$f")" "$(out_of b "$table" "$f")"; then
        echo >&2
        echo "the two binaries disagree on $f — there is no delta to read:" >&2
        echo "  A: $(cat "$(out_of a "$table" "$f")")" >&2
        echo "  B: $(cat "$(out_of b "$table" "$f")")" >&2
        exit 1
    fi
done < "$TMP/manifest"
echo

# ── timing: best of $RUNS, the binaries interleaved within each repeat ───────
time_one() {  # $1 = binary, $2 = image; echoes elapsed ns
    start=$(now_ns)
    "$1" --run "$2" >/dev/null 2>&1 </dev/null
    end=$(now_ns)
    echo $((end - start))
}

: > "$TMP/times"
printf 'timing (best of %s)' "$RUNS"
while IFS="$TAB" read -r table order name n f label; do
    printf '.'
    best_a=
    best_b=
    r=0
    while [ "$r" -lt "$RUNS" ]; do
        t=$(time_one "$BIN" "$(img_of a "$table" "$f")")
        { [ -z "$best_a" ] || [ "$t" -lt "$best_a" ]; } && best_a=$t
        if [ -n "$OTHER" ]; then
            t=$(time_one "$OTHER" "$(img_of b "$table" "$f")")
            { [ -z "$best_b" ] || [ "$t" -lt "$best_b" ]; } && best_b=$t
        fi
        r=$((r + 1))
    done
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$table" "$n" "$best_a" "${best_b:-0}" \
        "$order" "$label" >> "$TMP/times"
done < "$TMP/manifest"
echo

# ── report ──────────────────────────────────────────────────────────────────
# integer fixed point, one decimal: sh has no floats and this needs no library.
signed() {  # $1 = ns (may be negative), $2 = elements
    v=$1
    sign=+
    if [ "$v" -lt 0 ]; then sign=-; v=$((-v)); fi
    printf '%s%s.%s' "$sign" $((v / $2)) $((v * 10 / $2 % 10))
}

ms() { printf '%s.%sms' $(($1 / 1000000)) $(($1 * 10 / 1000000 % 10)); }

echo
if [ -n "$OTHER" ]; then
    printf 'A = %s\nB = %s\n' "$BIN" "$OTHER"
else
    printf '%s\n' "$BIN"
fi

table=
base_a=0
base_b=0
while IFS="$TAB" read -r t n ta tb order label; do
    if [ "$t" != "$table" ]; then
        table=$t
        base_a=$ta
        base_b=$tb
        echo
        printf '%s — %s elements, best of %s\n' "$t" "$n" "$RUNS"
        if [ -n "$OTHER" ]; then
            printf '  %-42s %11s %11s\n' '' A B
            printf '  %-42s %11s %11s   <- the machine, not the change\n' \
                "$label" "$(ms "$ta")" "$(ms "$tb")"
            # the baselines are the one pair that must agree for anything below
            # to be a comparison rather than two measurements.
            spread=$((ta - tb))
            [ "$spread" -lt 0 ] && spread=$((-spread))
            # 2ms as well as 3%, because a short baseline is 3% away from
            # itself: the threshold has to clear the clock, not just the ratio.
            if [ "$((spread * 100 / ta))" -ge 3 ] && [ "$spread" -ge 2000000 ]; then
                printf '  !! the baselines are %s%% apart: the columns did not see the same machine\n' \
                    "$((spread * 100 / ta))"
            fi
        else
            printf '  %-42s %11s   <- the machine, not the change\n' \
                "$label" "$(ms "$ta")"
        fi
        continue
    fi
    if [ -n "$OTHER" ]; then
        printf '  %-42s %11s %11s\n' "$label" \
            "$(signed $((ta - base_a)) "$n")" "$(signed $((tb - base_b)) "$n")"
    else
        printf '  %-42s %11s\n' "$label" "$(signed $((ta - base_a)) "$n")"
    fi
done < "$TMP/times"
echo
echo "every row is ns/elem over its own column's baseline; the baseline is wall time."
