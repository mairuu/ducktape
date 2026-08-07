#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct FunDef FunDef;
typedef struct Obj Obj;

// runtime value. VAL_OBJ payloads (strings, arrays) live on the GC heap
// (`object.h`); everything else is a plain value type.
typedef enum {
  VAL_INT,
  VAL_FLOAT,
  VAL_BOOL,
  VAL_CHAR, // a Unicode scalar value; see string_utils.h
  VAL_UNIT,
  VAL_RANGE,
  VAL_FUN, // top-level function (no captures)
  VAL_OBJ, // heap object: string, array
} ValueKind;

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

// renders a float the way the language spells one: the shortest form that
// reads back as the same double, always with a `.` or an exponent so a float
// never looks like an int. Writes at most 32 bytes; returns the length.
int value_format_float(double f, char *buf, size_t cap);

void value_print(Value v, FILE *out);

// deep equality: strings by pointer (interned), arrays elementwise.
bool value_equal(Value a, Value b);
