#pragma once

#include <stdbool.h>
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
    struct {
      int64_t start;
      int64_t end;
      bool inclusive;
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
static inline Value val_unit(void) { return (Value){.kind = VAL_UNIT}; }
static inline Value val_fun(FunDef *fun) {
  return (Value){.kind = VAL_FUN, .as.fun = fun};
}
static inline Value val_obj(Obj *obj) {
  return (Value){.kind = VAL_OBJ, .as.obj = obj};
}

void value_print(Value v, FILE *out);

// deep equality: strings by pointer (interned), arrays elementwise.
bool value_equal(Value a, Value b);
