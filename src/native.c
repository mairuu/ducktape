#include "native.h"
#include "chunk.h"
#include "object.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════════
// The registry
// ═══════════════════════════════════════════════════════════════════════════════
//
// Two tables, one declaration surface. `@native` names a C function reached
// through the ordinary OP_CALL; `@intrinsic` names an opcode the call lowers
// to inline. Which tier a std function wants is a codegen question — an
// intrinsic costs no call frame but cannot be a value, since there is no body
// to point a global slot at.
//
// Nothing here knows about types: the signature lives in the `.dt` file and is
// the checker's business, so a native's contract with C is only "these many
// values in, one value out". A mismatch between the two is a bug in std, not
// something a program can provoke — the checker has already agreed the call
// site matches the written signature.

// ───────────────────────────────────────────────────────────────────────────────
// std::io
// ───────────────────────────────────────────────────────────────────────────────

static Value n_io_print(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  value_print(args[0], stdout);
  fputc('\n', stdout);
  return val_unit();
}

// The read side, and the shape it takes is what milestone 102's rule leaves:
// a native cannot mint a `Result`, so the two answers leave by different doors.
// The bytes are *appended* to the caller's buffer — the one out-parameter a
// mutable object makes possible — and the return is the failure, empty when
// there was none. A successful read has no message, so the empty string is a
// sentinel nothing else can produce.
//
// Read in chunks rather than sized by `ftell`: a length is a property of a
// regular file, and a pipe or a /proc entry answers zero for it while still
// having every byte to give.
static Value n_io_read_file(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *path = val_as_string(args[0]);
  ObjBytes *out = val_as_bytes(args[1]);

  char msg[512];
  // `fopen` reads to the first NUL, so a path containing one names a different
  // file than the string does. Refused rather than silently truncated.
  if ((int)strlen(path->chars) != path->len) {
    snprintf(msg, sizeof msg, "cannot read a path containing a NUL byte");
    return val_obj(&heap_intern(ctx->heap, msg, (int)strlen(msg))->obj);
  }

  FILE *file = fopen(path->chars, "rb");
  if (file == NULL) {
    snprintf(msg, sizeof msg, "cannot read '%s': %s", path->chars,
             strerror(errno));
    return val_obj(&heap_intern(ctx->heap, msg, (int)strlen(msg))->obj);
  }

  // Restored on every failure path, so a partial read leaves the caller's
  // buffer as it found it: `Err` promises nothing was appended.
  int start = out->len;
  char chunk[8192];
  const char *failure = NULL;
  for (;;) {
    size_t got = fread(chunk, 1, sizeof chunk, file);
    if (got == 0) {
      if (ferror(file)) {
        failure = strerror(errno);
      }
      break;
    }
    if ((int64_t)out->len + (int64_t)got > INT_MAX) {
      failure = "file is too large";
      break;
    }
    // may collect; `out` is `args[1]`, still on the VM stack and so still
    // rooted, which is the whole of what the calling convention promises.
    heap_bytes_reserve(ctx->heap, out, out->len + (int)got);
    memcpy(out->bytes + out->len, chunk, got);
    out->len += (int)got;
  }
  fclose(file);

  if (failure != NULL) {
    out->len = start;
    snprintf(msg, sizeof msg, "cannot read '%s': %s", path->chars, failure);
    return val_obj(&heap_intern(ctx->heap, msg, (int)strlen(msg))->obj);
  }
  return val_obj(&heap_intern(ctx->heap, "", 0)->obj);
}

// ───────────────────────────────────────────────────────────────────────────────
// std::array
// ───────────────────────────────────────────────────────────────────────────────

// Two kinds live here. `push`/`pop` are what a `[T]` cannot express about
// itself: a slot that did not exist before, and one fewer than there was. The
// rest are what it *can* express and shouldn't — a bulk move whose ducktape
// spelling is an interpreted loop over a native call, which is all cost and no
// decision (milestone 106).
//
// A length arriving from a program is an `int64_t` and an array's `count` is an
// `int`, so every one of these checks the narrowing rather than casting through
// it: `(int)n` of 2^33 is 0, which would make an absurd request a silent no-op.
static bool array_len_ok(NativeCtx *ctx, int64_t n) {
  if (n < 0) {
    ctx->error = "array length cannot be negative";
    return false;
  }
  if (n > INT_MAX) {
    ctx->error = "array length is too large";
    return false;
  }
  return true;
}

static Value n_array_push(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjArray *arr = val_as_array(args[0]);
  // reserve may collect; both the array and the value being pushed are still
  // on the VM stack as `args`, which is the whole of the calling convention.
  // `count` rises only once the slot is really there.
  heap_array_reserve(ctx->heap, arr, arr->count + 1);
  arr->items[arr->count++] = args[1];
  return val_unit();
}

// Lowering `count` is what drops the value, so the array no longer roots it.
// It survives because the VM pushes the result with nothing allocating in
// between — the same window `string_slice`'s freshly interned result lives in.
//
// The empty check is this function's own contract rather than a diagnostic a
// program can reach: `pop_last` is private to `std::array`, whose only caller
// tests the length first so it can answer `None`.
static Value n_array_pop(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjArray *arr = val_as_array(args[0]);
  if (arr->count == 0) {
    ctx->error = "pop from an empty array";
    return val_unit();
  }
  return arr->items[--arr->count];
}

// The one native that *makes* an array, and it can because an array's shape
// belongs to the runtime — the m102 rule is not "a native cannot allocate" but
// "a native cannot reach a definition a program wrote", so a struct and an enum
// are out and a string, a buffer, an array and a tuple are in.
//
// It is also the first native to let a program name an allocation *size*.
// Everything else here is sized by data already in hand — `push` grows by one,
// `concat` sums two existing lengths — so `array_len_ok` is the whole of the
// difference, and above it the runtime's standing assumption that a malloc
// succeeds is the only thing left (see `heap_alloc`).
//
// Nothing allocates between the array existing and the last slot being written,
// so the half-filled array is never handed to a collection.
static Value n_array_fill(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  if (!array_len_ok(ctx, args[0].as.i)) {
    return val_unit();
  }
  int n = (int)args[0].as.i;
  ObjArray *arr = heap_array(ctx->heap, n);
  for (int i = 0; i < n; i++) {
    arr->items[i] = args[1];
  }
  return val_obj(&arr->obj);
}

static Value n_array_reserve(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  if (!array_len_ok(ctx, args[1].as.i)) {
    return val_unit();
  }
  heap_array_reserve(ctx->heap, val_as_array(args[0]), (int)args[1].as.i);
  return val_unit();
}

// Shortening is only `count`: the dropped values stay in the buffer but stop
// being traced, which is the same window `pop` leaves and is safe for the same
// reason — nothing reads past `count`, and `reserve` copies only that far.
// Growing is not this function's business, so a length at or above the current
// one is a no-op rather than an error.
static Value n_array_truncate(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  if (!array_len_ok(ctx, args[1].as.i)) {
    return val_unit();
  }
  ObjArray *arr = val_as_array(args[0]);
  int n = (int)args[1].as.i;
  if (n < arr->count) {
    arr->count = n;
  }
  return val_unit();
}

// `xs.extend(xs)` is a program a program may write, so the source buffer is
// read *after* the reserve: growing reallocates `items`, and when the two
// arrays are one object the buffer that moved is the one being copied from.
static Value n_array_extend(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjArray *arr = val_as_array(args[0]);
  int added = val_as_array(args[1])->count;
  if (added == 0) {
    return val_unit();
  }
  if (!array_len_ok(ctx, (int64_t)arr->count + added)) {
    return val_unit();
  }
  heap_array_reserve(ctx->heap, arr, arr->count + added);
  memcpy(arr->items + arr->count, val_as_array(args[1])->items,
         sizeof(Value) * (size_t)added);
  arr->count += added;
  return val_unit();
}

// ───────────────────────────────────────────────────────────────────────────────
// std::sort
// ───────────────────────────────────────────────────────────────────────────────

// The one native that runs *ducktape of its own*. Every other function in this
// file answers out of C alone; a sort cannot, because the order it sorts by is
// a value the program wrote — so each comparison is a call back across the
// boundary (`native_call`, see `native.h`). That makes this the native where
// the calling convention's quiet assumption stops holding: user code allocates,
// so a collection can happen *inside* this function, between any two elements.
//
// Two consequences shape everything below.
//
//   - **A C local is not a root.** The scratch the merge needs is two rooted
//     ObjArrays rather than a `Value[]` from the allocator: values living only
//     in C memory are invisible to the collector, and the first comparison that
//     triggered one would sweep half the array being sorted.
//   - **The array is sorted through a snapshot.** The comparator can reach the
//     very array it is ordering — `xs.sort_by(|a, b| { xs.push(a); ... })` is a
//     program a program may write — and a `push` reallocates `items` under any
//     pointer C is holding. The scratch arrays cannot be reached from ducktape
//     at all, so the merge walks those, and `arr` is touched only before the
//     first comparison and after the last.
//
// The algorithm is milestone 62's, unchanged and for its reasons: bottom-up,
// stable, one array of scratch (two here, since neither may be the caller's).

static int min_int(int a, int b) { return a < b ? a : b; }

// One comparison, in the program's own terms. `-1`/`0`/`1` rather than whatever
// the closure answered, so the merge below reads a *decision* — the same
// normalisation `string_cmp` does for the same reason.
//
// False means the comparator failed (a panic, a bad index, a failing call of
// its own). It has already reported itself, so there is nothing to add: the
// caller unwinds.
static bool sort_compare(NativeCtx *ctx, Value cmp, Value a, Value b,
                         int *order) {
  Value cmp_args[2] = {a, b};
  Value result;
  if (!native_call(ctx, cmp, cmp_args, 2, &result)) {
    return false;
  }
  *order = result.as.i < 0 ? -1 : (result.as.i > 0 ? 1 : 0);
  return true;
}

// `src[lo..mid)` and `src[mid..hi)` are each in order; write them merged into
// `dst[lo..hi)`. The left run wins ties, and that is the whole of what makes
// the sort stable — equal elements leave in the order the runs found them.
//
// Both pointers are into scratch no program can reach, which is what lets them
// be held across the comparison at all.
static bool sort_merge(NativeCtx *ctx, Value cmp, const Value *src, Value *dst,
                       int lo, int mid, int hi) {
  int i = lo, j = mid, k = lo;
  while (k < hi) {
    bool take_left;
    if (i >= mid) {
      take_left = false;
    } else if (j >= hi) {
      take_left = true;
    } else {
      int order;
      if (!sort_compare(ctx, cmp, src[i], src[j], &order)) {
        return false;
      }
      take_left = order <= 0;
    }
    dst[k++] = take_left ? src[i++] : src[j++];
  }
  return true;
}

static Value n_sort_by(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjArray *arr = val_as_array(args[0]);
  // a Value, not an ObjClosure: a comparator may be a closure literal *or* a
  // plain function passed by name, and `native_call` resolves both exactly as
  // OP_CALL does.
  Value cmp = args[1];

  int len = arr->count;
  if (len < 2) {
    return val_unit();
  }

  // `heap_array` fills with unit, so both are safe to trace from the moment
  // they exist; rooting each before allocating the next is what keeps the
  // second allocation from collecting the first.
  ObjArray *a = heap_array(ctx->heap, len);
  native_root(ctx, val_obj(&a->obj));
  ObjArray *b = heap_array(ctx->heap, len);
  native_root(ctx, val_obj(&b->obj));
  for (int i = 0; i < len; i++) {
    a->items[i] = arr->items[i];
  }

  Value *src = a->items;
  Value *dst = b->items;
  bool ok = true;

  for (int width = 1; width < len; width *= 2) {
    for (int lo = 0; lo < len;) {
      int mid = min_int(lo + width, len);
      int hi = min_int(lo + 2 * width, len);
      if (!sort_merge(ctx, cmp, src, dst, lo, mid, hi)) {
        ok = false;
        break;
      }
      lo = hi;
    }
    if (!ok) {
      break;
    }
    Value *swap = src;
    src = dst;
    dst = swap;
  }

  if (ok) {
    // The write-back, with no comparison in between — so `arr->items` is read
    // after the last thing that could have moved it. A comparator that changed
    // the array's *length* is refused rather than half-honoured: the sort holds
    // a snapshot of a sequence that no longer exists, and scattering it over
    // the array that replaced it would be worse than saying so. The array is
    // left exactly as it was.
    if (arr->count != len) {
      ctx->error = "the array changed length while it was being sorted";
    } else {
      for (int i = 0; i < len; i++) {
        arr->items[i] = src[i];
      }
    }
  }

  native_unroot(ctx, 2);
  return val_unit();
}

// ───────────────────────────────────────────────────────────────────────────────
// std::char
// ───────────────────────────────────────────────────────────────────────────────

// The one thing a `char` cannot express about itself is its number, so these
// two are the whole of `std::char`'s C surface. `is_digit`, `to_upper` and the
// rest are ordinary ducktape arithmetic on top of them — milestone 23's rule,
// and here it cuts deeper than usual, because a code point *is* an int and
// every classification is a range test over one.

static Value n_char_code(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(args[0].as.c);
}

// The inverse, and the only place a char can be conjured from nothing, so it
// is where the scalar-value invariant is enforced: a surrogate half and
// anything past U+10FFFF are not characters, and letting one through would
// mean `utf8_encode` could fail somewhere that cannot report it.
static Value n_char_from_code(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  int64_t code = args[0].as.i;
  if (code < 0 || code > 0x10FFFF || !utf8_is_scalar((uint32_t)code)) {
    ctx->error = "not a Unicode scalar value";
    return val_unit();
  }
  return val_char((uint32_t)code);
}

// ───────────────────────────────────────────────────────────────────────────────
// std::fmt
// ───────────────────────────────────────────────────────────────────────────────

// A float to a fixed number of decimals. This is the one rendering decision
// the language cannot express: `value_format_float` picks the shortest decimal
// that round-trips, which is the right default and the only thing `"{f}"` can
// say. Allocating, so `args` staying on the VM stack is what roots the
// arguments across the intern.
static Value n_fmt_float(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  int64_t precision = args[1].as.i;
  if (precision < 0 || precision > 30) {
    ctx->error = "precision must be between 0 and 30";
    return val_unit();
  }
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "%.*f", (int)precision, args[0].as.f);
  if (len < 0 || (size_t)len >= sizeof(buf)) {
    ctx->error = "formatted float is too long";
    return val_unit();
  }
  ObjString *out = heap_intern(ctx->heap, buf, len);
  return val_obj(&out->obj);
}

// ───────────────────────────────────────────────────────────────────────────────
// std::hash
// ───────────────────────────────────────────────────────────────────────────────

// These two were once the whole of what hashing could not say about itself in
// ducktape: the language had no bitwise operator at all, so a mixing function
// was not "slow to write" up there, it was *unwritable*, and the only mixer a
// program could express was the weak `h * 31 + x`.
//
// **Milestone 65 added the operators, so that argument has expired.** What
// keeps these here now is cost rather than capability: `hash_mix` is six
// operations of pure arithmetic with no callback into the interpreter, which is
// exactly the shape milestone 64's measurements say to move into C — a native
// call costs a few nanoseconds against roughly thirty for each interpreted
// step. tests/run/bitwise_mixer.dt writes this function out in ducktape,
// constant for constant, and checks the two agree on a thousand inputs; that
// twin is what keeps an optimisation from quietly becoming a black box.

// Fold `value` into `state`, one round. `value` is finalised through a
// splitmix64 step first so that a low-entropy int (0, 1, 2 — the commonest keys
// there are) still lands spread across all 64 bits, then FNV-1a's prime carries
// the accumulation and a last shift-xor moves the high bits down. That last
// step is what `%` needs: an unfinalised FNV state has its best entropy at the
// top, and the reduction to a slot index reads the bottom.
//
// Wrapping is intentional and every multiply here relies on it — unsigned in C
// so it is defined rather than merely what the hardware does; the int the
// language hands back wraps the same way (`2^62 * 4 == 0`).
static Value n_hash_mix(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  uint64_t h = (uint64_t)args[0].as.i;
  uint64_t v = (uint64_t)args[1].as.i;
  v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33;
  h ^= v;
  h *= 0x100000001b3ULL; // FNV-1a's 64-bit prime
  h ^= h >> 29;
  return val_int((int64_t)h);
}

// A string's hash is a *field read*: the heap hashed these bytes already when
// it interned them, and kept the result to find the string's bucket with. So
// the one type whose hash would otherwise be a walk over its bytes is the one
// type that costs nothing — and `impl Hash for string` is the cheapest impl in
// std rather than the dearest.
//
// It is a `uint32_t` widened into an int, so it is always non-negative here;
// the sign only appears once `hash_mix` has folded it. The value is FNV-1a over
// the bytes, so equal Strings hash equal because they are the same pointer, and
// unequal ones by the same argument the intern table already relies on.
static Value n_hash_string(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int((int64_t)val_as_string(args[0])->hash);
}

// ───────────────────────────────────────────────────────────────────────────────
// std::panic
// ───────────────────────────────────────────────────────────────────────────────

// The whole of a panic: set `ctx->error` and let the VM report it at the call
// site. There is no unwinding, so this is the last thing the program does.
//
// The message is the argument's own bytes rather than a copy: an ObjString is
// NUL-terminated, and `args` still sits on the VM stack when the VM reads
// `ctx->error` — nothing can collect in between. Aliasing the heap is only
// safe *because* a panic never returns.
static Value n_panic_abort(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ctx->error = val_as_string(args[0])->chars;
  return val_unit();
}

// ───────────────────────────────────────────────────────────────────────────────
// std::string
// ───────────────────────────────────────────────────────────────────────────────

static Value n_string_len(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(val_as_string(args[0])->len);
}

// Byte order, which is the one thing interning gives nothing towards. Two equal
// strings are one pointer, so `==` is a pointer compare — but pointer *order*
// is allocation order, which is arbitrary and differs run to run. So the table
// hands this function exactly one shortcut, the equal case, and the rest is a
// real walk over the bytes.
//
// `memcmp` compares as unsigned char, so this is code-point order for anything
// well-formed in UTF-8: a multi-byte sequence's lead byte is above every ASCII
// byte, and sorts against another lead byte the same way the code points do.
//
// Normalised to -1/0/1 rather than passed through: `memcmp`'s magnitude is
// implementation-defined, and a program can print what `cmp` answers.
static Value n_string_cmp(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  ObjString *a = val_as_string(args[0]);
  ObjString *b = val_as_string(args[1]);
  if (a == b) {
    return val_int(0);
  }
  int shared = a->len < b->len ? a->len : b->len;
  int order = memcmp(a->chars, b->chars, (size_t)shared);
  if (order == 0) {
    // one is a prefix of the other, so the shorter sorts first. They cannot be
    // the same length here: equal bytes at equal length would be one pointer.
    order = a->len < b->len ? -1 : 1;
  }
  return val_int(order < 0 ? -1 : 1);
}

// The crossing between the two views of text: a string is bytes, a char is a
// scalar value, and these are where one becomes the other. The crossing is a
// *walk* rather than an index — `std::iter`'s `CharIter` is the only
// caller of the two below, and it is what keeps the byte offsets they speak in
// out of every API a program can see (they are private free functions there,
// for `range_start`'s reason).
//
// Neither allocates, so neither has anything to say about the calling
// convention: a char is a value and an offset is an int.

// The character beginning at byte offset `at`. Since milestone 101 a string is
// valid UTF-8 by construction, so the decode cannot fail on the *bytes*: the
// one way this errors past the bounds check is an `at` in the middle of a
// sequence, which is a question about the offset, not about the string.
static Value n_string_char_at(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t at = args[1].as.i;
  if (at < 0 || at >= s->len) {
    ctx->error = "string offset is out of bounds";
    return val_unit();
  }
  uint32_t cp;
  if (utf8_decode(s->chars + at, s->len - (int)at, &cp) == 0) {
    ctx->error = "string offset is not a character boundary";
    return val_unit();
  }
  return val_char(cp);
}

// Where the character *ending* at byte offset `at` begins — the one step a
// walk cannot take on its own. Going forwards, the width is recoverable from
// the character just read (a scalar value determines its encoding), so
// `std::iter` does that step in ducktape arithmetic; going backwards there is
// no character in hand yet, and the only thing that says where one starts is
// the encoding's tag on the bytes themselves. That is what a continuation byte
// is for, and this is the whole of what reads it.
//
// A sequence is at most four bytes, so the scan is bounded rather than a
// search: past that, or if the lead byte found does not span exactly to `at`,
// `at` was not a boundary — the same reading the forward direction gives, and
// since milestone 101 the only one left, the bytes themselves being valid.
static Value n_string_prev_boundary(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t at = args[1].as.i;
  if (at <= 0 || at > s->len) {
    ctx->error = "string offset is out of bounds";
    return val_unit();
  }
  int64_t start = at - 1;
  while (start > 0 && at - start < 4 &&
         ((uint8_t)s->chars[start] & 0xC0) == 0x80) {
    start--;
  }
  uint32_t cp;
  int width = utf8_decode(s->chars + start, s->len - (int)start, &cp);
  if (width == 0 || start + width != at) {
    ctx->error = "string offset is not a character boundary";
    return val_unit();
  }
  return val_int(start);
}

// How many characters `s` is. A count and not the characters themselves,
// which is the shape of what `std::string` still needs: `char_width` backs the
// `pad_*` lang items, and `std::string` sits on the wrong side of the import
// graph to reach `CharIter` (`std::cmp` imports `std::string`, so the reverse
// edge would be a cycle). Nothing is allocated — strictly less than the array
// the pads used to build to count it.
//
// This is the only native that decodes the *whole* string, so it is also the
// cheapest detector the string invariant has: the guard below is unreachable
// from any program, and a firing means the runtime built a string it should
// not have. It stays because dropping it would turn that bug into a hang.
static Value n_string_char_count(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t count = 0;
  for (int i = 0; i < s->len;) {
    uint32_t cp;
    int width = utf8_decode(s->chars + i, s->len - i, &cp);
    if (width == 0) {
      ctx->error = "string is not valid UTF-8";
      return val_unit();
    }
    i += width;
    count++;
  }
  return val_int(count);
}

// Does `needle` sit at byte offset `at`? The compare a string has no other way
// to ask for: `std::text` used to cut a candidate window out and compare
// *that*, which allocated an interned string per position and — since milestone
// 101 — would now fail outright, because a window cut at an arbitrary byte
// offset is exactly the slice a valid string refuses to make.
//
// It is total where that cut is not, and the encoding is what makes it so: a
// non-empty needle is valid UTF-8, so its first byte is never a continuation
// byte, so a compare at a mid-sequence offset simply says no. Only `at` itself
// is bounds-checked; running off the end is an answer, not an error.
static Value n_string_matches_at(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t at = args[1].as.i;
  ObjString *needle = val_as_string(args[2]);
  if (at < 0 || at > s->len) {
    ctx->error = "string offset is out of bounds";
    return val_bool(false);
  }
  if (needle->len > s->len - at) {
    return val_bool(false);
  }
  return val_bool(memcmp(s->chars + at, needle->chars, (size_t)needle->len) ==
                  0);
}

// `s[from..to)`, in bytes. Allocating, so it is the one that has to obey the
// calling convention: `heap_intern` can collect, and it is `args` still
// sitting on the VM stack that keeps the source string alive across it.
//
// The boundary check is what makes every string valid UTF-8 (milestone 101):
// this is the only operation that can produce bytes no intake vetted, so the
// invariant costs two O(1) tests here and nothing anywhere else. What used to
// be a malformed string travelling until something decoded it is now an error
// at the cut that made it.
static Value n_string_slice(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t from = args[1].as.i;
  int64_t to = args[2].as.i;
  if (from < 0 || to < from || to > s->len) {
    ctx->error = "string slice is out of bounds";
    return val_unit();
  }
  if (!utf8_is_boundary(s->chars, s->len, (int)from) ||
      !utf8_is_boundary(s->chars, s->len, (int)to)) {
    ctx->error = "string slice does not cut at a character boundary";
    return val_unit();
  }
  ObjString *out = heap_intern(ctx->heap, s->chars + from, (int)(to - from));
  return val_obj(&out->obj);
}

// A `StringBuf` is the object a string cannot be: uninterned, so it may be
// appended to in place. These natives are the whole of what it cannot express
// about itself — existing, growing, emptying, its length, and becoming a
// string — and `join`/`concat`/`repeat` are ordinary ducktape on top of them.

static Value n_strbuf_new(NativeCtx *ctx, Value *args, int argc) {
  (void)args;
  (void)argc;
  ObjStrBuf *buf = heap_strbuf(ctx->heap);
  return val_obj(&buf->obj);
}

// Append the bytes of a string. The buffer is `args[0]`, still on the VM stack,
// which is what keeps it rooted across the collection `reserve` may trigger —
// and the source string is `args[1]` for the same reason. `len` rises only
// once the bytes are really there, mirroring `array_push`; here that is about
// what `build` will copy rather than about what the collector will trace,
// since bytes are never traced at all.
static Value n_strbuf_push(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjStrBuf *buf = val_as_strbuf(args[0]);
  ObjString *s = val_as_string(args[1]);
  if (s->len == 0) {
    return val_unit();
  }
  heap_strbuf_reserve(ctx->heap, buf, buf->len + s->len);
  memcpy(buf->bytes + buf->len, s->chars, (size_t)s->len);
  buf->len += s->len;
  return val_unit();
}

// Append one char's UTF-8 bytes. This is the append a `[string]` of parts
// could never have offered — the reason a buffer was worth a new object kind
// at all — since it puts bytes in without interning a string to hold them.
// It is also what lets `from_chars` be ordinary ducktape rather than a fifth
// native: a string is built out of chars the same way it is built out of
// anything else.
static Value n_strbuf_push_char(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjStrBuf *buf = val_as_strbuf(args[0]);
  char bytes[UTF8_MAX_BYTES];
  int n = utf8_encode(args[1].as.c, bytes); // a char is always a scalar value
  heap_strbuf_reserve(ctx->heap, buf, buf->len + n);
  memcpy(buf->bytes + buf->len, bytes, (size_t)n);
  buf->len += n;
  return val_unit();
}

static Value n_strbuf_len(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(val_as_strbuf(args[0])->len);
}

// Empty the buffer without releasing what it grew to. Dropping `len` to 0 is
// the whole of it: the bytes past it are dead, `build` copies only up to `len`,
// and the next `push` overwrites them — so the capacity survives to be reused,
// which is the point, a buffer reused across a loop instead of a fresh one each
// round. Non-allocating, so it is the one strbuf native with no rooting
// concern.
static Value n_strbuf_clear(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  val_as_strbuf(args[0])->len = 0;
  return val_unit();
}

// Append an int's decimal digits. Without it a number has to be interned first
// (`push(b, "{n}")` builds a throwaway string), which is exactly the
// re-interning a buffer exists to avoid; here the digits go straight in, like
// `push_char`. `snprintf` cannot overrun `buf` — an int64_t is at most 20
// digits and a sign — so the length it returns is the real byte count, no clamp
// needed.
static Value n_strbuf_push_int(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjStrBuf *buf = val_as_strbuf(args[0]);
  char digits[24];
  int n = snprintf(digits, sizeof(digits), "%lld", (long long)args[1].as.i);
  heap_strbuf_reserve(ctx->heap, buf, buf->len + n);
  memcpy(buf->bytes + buf->len, digits, (size_t)n);
  buf->len += n;
  return val_unit();
}

// The one-way door: the bytes enter the intern table and stop being editable.
// Non-destructive — the buffer is unchanged and may be pushed to again — so
// this is a copy, which is also the only thing it *could* be: an ObjString is
// a flexible-array allocation of its own exact length.
static Value n_strbuf_build(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjStrBuf *buf = val_as_strbuf(args[0]);
  ObjString *out =
      heap_intern(ctx->heap, buf->bytes != NULL ? buf->bytes : "", buf->len);
  return val_obj(&out->obj);
}

// ───────────────────────────────────────────────────────────────────────────────
// std::bytes
// ───────────────────────────────────────────────────────────────────────────────
//
// The same buffer as a StringBuf with the invariant taken off. What a program
// names is an `int`, so both of a byte's edges have to be checked here: the
// value is 0..255, and there is no `byte` type to have checked it earlier.

static bool byte_ok(NativeCtx *ctx, int64_t v) {
  if (v < 0 || v > 255) {
    ctx->error = "byte value is out of range";
    return false;
  }
  return true;
}

// An index arriving from a program is an int64_t against an int `len`, so the
// comparison is done wide rather than narrowed into one — `(int)` of 2^32 is 0,
// which would make an absurd index read element zero.
static bool byte_index_ok(NativeCtx *ctx, ObjBytes *b, int64_t i) {
  if (i < 0 || i >= b->len) {
    ctx->error = "byte index is out of bounds";
    return false;
  }
  return true;
}

static Value n_bytes_new(NativeCtx *ctx, Value *args, int argc) {
  (void)args;
  (void)argc;
  return val_obj(&heap_bytes(ctx->heap)->obj);
}

static Value n_bytes_len(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(val_as_bytes(args[0])->len);
}

static Value n_bytes_get(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjBytes *b = val_as_bytes(args[0]);
  if (!byte_index_ok(ctx, b, args[1].as.i)) {
    return val_unit();
  }
  return val_int((unsigned char)b->bytes[args[1].as.i]);
}

static Value n_bytes_set(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjBytes *b = val_as_bytes(args[0]);
  if (!byte_index_ok(ctx, b, args[1].as.i) || !byte_ok(ctx, args[2].as.i)) {
    return val_unit();
  }
  b->bytes[args[1].as.i] = (char)(unsigned char)args[2].as.i;
  return val_unit();
}

// `len` rises only once the byte is really there, the same ordering every other
// buffer append keeps — and here, as for a StringBuf, it protects what a reader
// would copy rather than what the collector would trace.
static Value n_bytes_push(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjBytes *b = val_as_bytes(args[0]);
  if (!byte_ok(ctx, args[1].as.i)) {
    return val_unit();
  }
  heap_bytes_reserve(ctx->heap, b, b->len + 1);
  b->bytes[b->len++] = (char)(unsigned char)args[1].as.i;
  return val_unit();
}

static Value n_bytes_clear(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  val_as_bytes(args[0])->len = 0;
  return val_unit();
}

// The exit door's question, asked separately from the exit itself: a native
// cannot mint an `Option` (it has no handle on the VariantDef), so the choice
// is made in ducktape and this is the predicate it chooses on.
static Value n_bytes_is_utf8(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  ObjBytes *b = val_as_bytes(args[0]);
  return val_bool(b->len == 0 || utf8_validate(b->bytes, b->len));
}

// The third intake, and the first one a *program* controls. A string is
// guaranteed valid UTF-8 (milestone 101), so this is where a Bytes stops being
// arbitrary — the check is not a formality but the whole reason the two types
// are different. `is_utf8` is the safe way in; this refuses rather than trusts,
// so the invariant holds even when the raw native is bound directly.
static Value n_bytes_to_string(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjBytes *b = val_as_bytes(args[0]);
  if (b->len > 0 && !utf8_validate(b->bytes, b->len)) {
    ctx->error = "bytes are not valid UTF-8";
    return val_unit();
  }
  ObjString *out =
      heap_intern(ctx->heap, b->bytes != NULL ? b->bytes : "", b->len);
  return val_obj(&out->obj);
}

// The other direction needs no check at all, which is the asymmetry stated as
// code: every string is already valid UTF-8, so its encoding is always a legal
// Bytes while the reverse is only sometimes a legal string.
static Value n_string_to_utf8(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  ObjBytes *b = heap_bytes(ctx->heap);
  if (s->len > 0) {
    // `b` is unreachable from any root, so it must not outlive an allocation it
    // does not survive: `reserve` is the only one here, and it happens before
    // anything is written rather than between two writes.
    native_root(ctx, val_obj(&b->obj));
    heap_bytes_reserve(ctx->heap, b, s->len);
    native_unroot(ctx, 1);
    memcpy(b->bytes, s->chars, (size_t)s->len);
    b->len = s->len;
  }
  return val_obj(&b->obj);
}

// ───────────────────────────────────────────────────────────────────────────────
// Tables
// ───────────────────────────────────────────────────────────────────────────────

typedef struct {
  const char *name;
  NativeFn fn;
} NativeEntry;

static const NativeEntry natives[] = {
    {"array_extend", n_array_extend},
    {"array_fill", n_array_fill},
    {"array_pop", n_array_pop},
    {"array_push", n_array_push},
    {"array_reserve", n_array_reserve},
    {"array_truncate", n_array_truncate},
    {"bytes_clear", n_bytes_clear},
    {"bytes_get", n_bytes_get},
    {"bytes_is_utf8", n_bytes_is_utf8},
    {"bytes_len", n_bytes_len},
    {"bytes_new", n_bytes_new},
    {"bytes_push", n_bytes_push},
    {"bytes_set", n_bytes_set},
    {"bytes_to_string", n_bytes_to_string},
    {"char_code", n_char_code},
    {"char_from_code", n_char_from_code},
    {"fmt_float", n_fmt_float},
    {"hash_mix", n_hash_mix},
    {"hash_string", n_hash_string},
    {"io_print", n_io_print},
    {"io_read_file", n_io_read_file},
    {"panic_abort", n_panic_abort},
    {"sort_by", n_sort_by},
    {"strbuf_build", n_strbuf_build},
    {"strbuf_clear", n_strbuf_clear},
    {"strbuf_len", n_strbuf_len},
    {"strbuf_new", n_strbuf_new},
    {"strbuf_push", n_strbuf_push},
    {"strbuf_push_char", n_strbuf_push_char},
    {"strbuf_push_int", n_strbuf_push_int},
    {"string_char_at", n_string_char_at},
    {"string_char_count", n_string_char_count},
    {"string_cmp", n_string_cmp},
    {"string_len", n_string_len},
    {"string_matches_at", n_string_matches_at},
    {"string_prev_boundary", n_string_prev_boundary},
    {"string_slice", n_string_slice},
    {"string_to_utf8", n_string_to_utf8},
};

// An intrinsic's opcode must take **no operand bytes** and must pop exactly
// the declared parameters and push exactly the declared return: `compile_call`
// emits it bare after the arguments, with nothing to encode an operand into
// and no frame to reconcile a mismatch against. That is a constraint on which
// opcodes can be exposed this way, not on the declaration surface — anything
// needing an operand belongs on the @native side.
typedef struct {
  const char *name;
  OpCode op;
} IntrinsicEntry;

static const IntrinsicEntry intrinsics[] = {
    {"array_len", OP_LEN},
    {"range_start", OP_RANGE_START},
    {"range_stop", OP_RANGE_STOP},
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

NativeFn native_lookup(StringView name) {
  for (int i = 0; i < COUNT_OF(natives); i++) {
    if (sv_equal_cstr(name, natives[i].name)) {
      return natives[i].fn;
    }
  }
  return NULL;
}

int intrinsic_lookup(StringView name) {
  for (int i = 0; i < COUNT_OF(intrinsics); i++) {
    if (sv_equal_cstr(name, intrinsics[i].name)) {
      return (int)intrinsics[i].op;
    }
  }
  return -1;
}

// built once into a static buffer; the caller only ever prints it.
static const char *join_names(char *buf, size_t cap, const char *const *names,
                              int count) {
  size_t n = 0;
  for (int i = 0; i < count && n + 1 < cap; i++) {
    int w = snprintf(buf + n, cap - n, "%s%s", i > 0 ? ", " : "", names[i]);
    if (w < 0 || (size_t)w >= cap - n) {
      break;
    }
    n += (size_t)w;
  }
  return buf;
}

const char *native_names(void) {
  static char buf[1024];
  const char *names[COUNT_OF(natives)];
  for (int i = 0; i < COUNT_OF(natives); i++) {
    names[i] = natives[i].name;
  }
  return join_names(buf, sizeof buf, names, COUNT_OF(natives));
}

const char *intrinsic_names(void) {
  static char buf[512];
  const char *names[COUNT_OF(intrinsics)];
  for (int i = 0; i < COUNT_OF(intrinsics); i++) {
    names[i] = intrinsics[i].name;
  }
  return join_names(buf, sizeof buf, names, COUNT_OF(intrinsics));
}
