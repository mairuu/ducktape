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
// std::string
// ───────────────────────────────────────────────────────────────────────────────

static Value n_string_len(NativeCtx *ctx, Value *args, int argc) {
  (void)ctx;
  (void)argc;
  return val_int(val_as_string(args[0])->len);
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

// ───────────────────────────────────────────────────────────────────────────────
// Tables
// ───────────────────────────────────────────────────────────────────────────────

typedef struct {
  const char *name;
  NativeFn fn;
} NativeEntry;

static const NativeEntry natives[] = {
    {"io_print", n_io_print},
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
  static char buf[256];
  const char *names[COUNT_OF(natives)];
  for (int i = 0; i < COUNT_OF(natives); i++) {
    names[i] = natives[i].name;
  }
  return join_names(buf, sizeof buf, names, COUNT_OF(natives));
}

const char *intrinsic_names(void) {
  static char buf[256];
  const char *names[COUNT_OF(intrinsics)];
  for (int i = 0; i < COUNT_OF(intrinsics); i++) {
    names[i] = intrinsics[i].name;
  }
  return join_names(buf, sizeof buf, names, COUNT_OF(intrinsics));
}
