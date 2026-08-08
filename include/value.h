#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct FunDef FunDef;
typedef struct Obj Obj;
typedef struct VariantDef VariantDef;

// how much of `as.inl.variant` is the field's kind rather than the pointer.
#define VAL_INL_KIND_MASK 7u

// runtime value. VAL_OBJ payloads (strings, arrays) live on the GC heap
// (`object.h`); everything else is a plain value type.
typedef enum {
  VAL_INT,
  VAL_FLOAT,
  VAL_BOOL,
  VAL_CHAR, // a Unicode scalar value; see string_utils.h
  VAL_UNIT,
  VAL_RANGE,
  VAL_FUN,     // top-level function (no captures)
  VAL_OBJ,     // heap object: string, array
  VAL_VARIANT, // a one-field enum variant carried inline; see below
} ValueKind;

// VAL_VARIANT must stay last: every kind an inline variant's *field* can have
// is therefore below it, and `val_variant` needs them all to fit the three low
// bits of a pointer.
static_assert(VAL_VARIANT <= VAL_INL_KIND_MASK + 1,
              "an inline variant's field kind must fit VAL_INL_KIND_MASK");

typedef struct {
  ValueKind kind;
  union {
    int64_t i;
    double f;
    bool b;
    uint32_t c;
    // always half-open (`..=` normalised at OP_RANGE); the widest arm, so
    // this is what sets sizeof(Value).
    struct {
      int64_t start;
      int64_t end;
    } range;
    FunDef *fun;
    Obj *obj;
    // a one-field variant with no heap object behind it: a `VariantDef *` with
    // the field's own ValueKind in its three spare low bits, plus the field's
    // bits. Word-sized arms only — the pointer has taken the other half.
    struct {
      uintptr_t variant;
      union {
        int64_t i;
        double f;
        bool b;
        uint32_t c;
        FunDef *fun;
        Obj *obj;
      } field;
    } inl;
  } as;
} Value;

static inline Value val_int(int64_t i) {
  return (Value){.kind = VAL_INT, .as.i = i};
}
static inline Value val_float(double f) {
  return (Value){.kind = VAL_FLOAT, .as.f = f};
}
static inline Value val_bool(bool b) {
  return (Value){.kind = VAL_BOOL, .as.b = b};
}
static inline Value val_char(uint32_t c) {
  return (Value){.kind = VAL_CHAR, .as.c = c};
}
static inline Value val_unit(void) { return (Value){.kind = VAL_UNIT}; }
static inline Value val_fun(FunDef *fun) {
  return (Value){.kind = VAL_FUN, .as.fun = fun};
}
static inline Value val_obj(Obj *obj) {
  return (Value){.kind = VAL_OBJ, .as.obj = obj};
}

// A one-field variant needs no heap object; `runtime.md` "A one-field variant
// needs no object" has the argument, including why the field's kind rides in
// the pointer instead of the four spare bytes beside `kind`. Do not move it
// there — it is ~20ns per loop iteration in programs with no variant in them.

// excluding VAL_VARIANT is what bounds the nesting (a field is never itself
// inline) and what keeps every field's kind under 8, so it fits the low bits.
static inline bool val_variant_fits(Value field) {
  return field.kind != VAL_RANGE && field.kind != VAL_VARIANT;
}

static inline Value val_variant(VariantDef *variant, Value field) {
  // the static assert in value.c says a VariantDef *may* be aligned enough;
  // this says the allocator that handed out this one was.
  assert(((uintptr_t)variant & VAL_INL_KIND_MASK) == 0 &&
         "a VariantDef under-aligned for the field-kind bits");
  Value v = {.kind = VAL_VARIANT,
             .as.inl.variant = (uintptr_t)variant | (uintptr_t)field.kind};
  // the bits, not a named arm: which arm it is is the kind in those low bits.
  memcpy(&v.as.inl.field, &field.as, sizeof(v.as.inl.field));
  return v;
}

static inline VariantDef *val_variant_def(Value v) {
  return (VariantDef *)(v.as.inl.variant & ~(uintptr_t)VAL_INL_KIND_MASK);
}

// the field back out, as the Value that went in. The initializer zeroes the
// arms the copy does not reach, so the result is as determinate as any other.
static inline Value val_variant_field(Value v) {
  Value field = {.kind = (ValueKind)(v.as.inl.variant & VAL_INL_KIND_MASK)};
  memcpy(&field.as, &v.as.inl.field, sizeof(v.as.inl.field));
  return field;
}

// renders a float the way the language spells one: the shortest form that
// reads back as the same double, always with a `.` or an exponent so a float
// never looks like an int. Writes at most 32 bytes; returns the length.
int value_format_float(double f, char *buf, size_t cap);

void value_print(Value v, FILE *out);

// deep equality: strings by pointer (interned), arrays elementwise.
bool value_equal(Value a, Value b);
