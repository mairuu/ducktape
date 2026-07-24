#include "native.h"
#include "chunk.h"
#include "object.h"

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

// ───────────────────────────────────────────────────────────────────────────────
// std::array
// ───────────────────────────────────────────────────────────────────────────────

// The two operations a `[T]` cannot express about itself: a slot that did not
// exist before, and one fewer than there was. Everything else `std::array`
// offers is written in ducktape on top of these — which is why `pop` here is
// the raw, panicking one and the `Option`-returning `pop` lives in the .dt.

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

// ───────────────────────────────────────────────────────────────────────────────
// std::char
// ───────────────────────────────────────────────────────────────────────────────

// The one thing a `Char` cannot express about itself is its number, so these
// two are the whole of `std::char`'s C surface. `is_digit`, `to_upper` and the
// rest are ordinary ducktape arithmetic on top of them — milestone 23's rule,
// and here it cuts deeper than usual, because a code point *is* an Int and
// every classification is a range test over one.

static Value n_char_code(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(args[0].as.c);
}

// The inverse, and the only place a Char can be conjured from nothing, so it
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

// A Float to a fixed number of decimals. This is the one rendering decision
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

// The crossing between the two views of text: a String is bytes, a Char is a
// scalar value, and this is where one becomes the other. It is a *conversion*
// rather than an index on purpose — there is no `char_at(s, i)`, because a
// byte offset is not a character position and offering that spelling would
// make confusing the two the API's default.
//
// Two passes: the first counts and validates, so a malformed string is
// reported before anything is allocated, and the second fills an array whose
// size is then already known. `heap_array` may collect, but the source string
// is `args[0]` and still on the VM stack; nothing after it allocates at all,
// since a Char is a value.
static Value n_string_chars(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);

  int count = 0;
  for (int i = 0; i < s->len;) {
    uint32_t cp;
    int width = utf8_decode(s->chars + i, s->len - i, &cp);
    if (width == 0) {
      // reachable from ducktape: `slice` is indexed in bytes, so it can cut a
      // multi-byte sequence in half. A String is a byte string; only a Char
      // promises to be a scalar value.
      ctx->error = "string is not valid UTF-8";
      return val_unit();
    }
    i += width;
    count++;
  }

  ObjArray *out = heap_array(ctx->heap, count);
  int at = 0;
  for (int i = 0; i < s->len;) {
    uint32_t cp;
    i += utf8_decode(s->chars + i, s->len - i, &cp);
    out->items[at++] = val_char(cp);
  }
  return val_obj(&out->obj);
}

// `s[from..to)`, in bytes. Allocating, so it is the one that has to obey the
// calling convention: `heap_intern` can collect, and it is `args` still
// sitting on the VM stack that keeps the source string alive across it.
static Value n_string_slice(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjString *s = val_as_string(args[0]);
  int64_t from = args[1].as.i;
  int64_t to = args[2].as.i;
  if (from < 0 || to < from || to > s->len) {
    ctx->error = "string slice is out of bounds";
    return val_unit();
  }
  ObjString *out = heap_intern(ctx->heap, s->chars + from, (int)(to - from));
  return val_obj(&out->obj);
}

// A `StringBuf` is the object a String cannot be: uninterned, so it may be
// appended to in place. These natives are the whole of what it cannot express
// about itself — existing, growing, emptying, its length, and becoming a
// String — and `join`/`concat`/`repeat` are ordinary ducktape on top of them.

static Value n_strbuf_new(NativeCtx *ctx, Value *args, int argc) {
  (void)args;
  (void)argc;
  ObjStrBuf *buf = heap_strbuf(ctx->heap);
  return val_obj(&buf->obj);
}

// Append the bytes of a String. The buffer is `args[0]`, still on the VM stack,
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

// Append one Char's UTF-8 bytes. This is the append a `[String]` of parts
// could never have offered — the reason a buffer was worth a new object kind
// at all — since it puts bytes in without interning a String to hold them.
// It is also what lets `from_chars` be ordinary ducktape rather than a fifth
// native: a String is built out of Chars the same way it is built out of
// anything else.
static Value n_strbuf_push_char(NativeCtx *ctx, Value *args, int argc) {
  (void)argc;
  ObjStrBuf *buf = val_as_strbuf(args[0]);
  char bytes[UTF8_MAX_BYTES];
  int n = utf8_encode(args[1].as.c, bytes); // a Char is always a scalar value
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

// Append an Int's decimal digits. Without it a number has to be interned first
// (`push(b, "{n}")` builds a throwaway String), which is exactly the
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
// Tables
// ───────────────────────────────────────────────────────────────────────────────

typedef struct {
  const char *name;
  NativeFn fn;
} NativeEntry;

static const NativeEntry natives[] = {
    {"array_pop", n_array_pop},
    {"array_push", n_array_push},
    {"char_code", n_char_code},
    {"char_from_code", n_char_from_code},
    {"fmt_float", n_fmt_float},
    {"io_print", n_io_print},
    {"panic_abort", n_panic_abort},
    {"strbuf_build", n_strbuf_build},
    {"strbuf_clear", n_strbuf_clear},
    {"strbuf_len", n_strbuf_len},
    {"strbuf_new", n_strbuf_new},
    {"strbuf_push", n_strbuf_push},
    {"strbuf_push_char", n_strbuf_push_char},
    {"strbuf_push_int", n_strbuf_push_int},
    {"string_chars", n_string_chars},
    {"string_cmp", n_string_cmp},
    {"string_len", n_string_len},
    {"string_slice", n_string_slice},
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
  static char buf[512];
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
