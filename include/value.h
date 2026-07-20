#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct FunDef FunDef;

// runtime value. heap objects (strings, arrays, structs, closures) arrive
// with the GC milestone; everything here is a plain value type.
typedef enum {
  VAL_INT,
  VAL_FLOAT,
  VAL_BOOL,
  VAL_UNIT,
  VAL_RANGE,
  VAL_FUN, // top-level function (no captures)
} ValueKind;

typedef struct {
  ValueKind kind;
  union {
    int64_t i;
    double f;
    bool b;
    struct {
      int64_t start;
      int64_t end;
      bool inclusive;
    } range;
    FunDef *fun;
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
static inline Value val_unit(void) { return (Value){.kind = VAL_UNIT}; }
static inline Value val_fun(FunDef *fun) {
  return (Value){.kind = VAL_FUN, .as.fun = fun};
}

void value_print(Value v, FILE *out);
