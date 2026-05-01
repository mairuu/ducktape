#include "value.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "object.h"

bool val_equal(Value a, Value b) {
  if (a.type != b.type) {
    return false;
  }

  switch (a.type) {
  case VAL_EMPTY:
    return true;
  case VAL_NIL:
    return true;
  case VAL_BOOL:
    return AS_BOOL(a) == AS_BOOL(b);
  case VAL_NUMBER:
    return AS_NUMBER(a) == AS_NUMBER(b);
  case VAL_OBJECT:
    return obj_equal(AS_OBJECT(a), AS_OBJECT(b));
  }
  assert(false && "unknown value type");
  return false;
}

bool val_is_falsey(Value value) {
  switch (value.type) {
  case VAL_EMPTY:
    return true;
  case VAL_NIL:
    return true;
  case VAL_BOOL:
    return !AS_BOOL(value);
  case VAL_NUMBER:
    return AS_NUMBER(value) == 0;
  case VAL_OBJECT:
    return false;
  }
  assert(false && "unknown value type");
  return false;
}

static const char *val_type_names[] = {
    [VAL_EMPTY] = "empty",   [VAL_NIL] = "nil",       [VAL_BOOL] = "bool",
    [VAL_NUMBER] = "number", [VAL_OBJECT] = "object",
};

const char *val_type_name(ValueType type) {
  if (type < 0 || type >= sizeof(val_type_names) / sizeof(val_type_names[0])) {
    return "unknown";
  }
  return val_type_names[type];
}

void val_print(Value value) {
  char buf[64];
  val_sprintf(buf, sizeof(buf), value);
  printf("%s", buf);
}

int val_sprintf(char *buf, int bufsize, Value value) {
  switch (value.type) {
  case VAL_EMPTY:
    return snprintf(buf, bufsize, "<empty>");
  case VAL_NIL:
    return snprintf(buf, bufsize, "nil");
  case VAL_BOOL:
    return snprintf(buf, bufsize, AS_BOOL(value) ? "true" : "false");
  case VAL_NUMBER:
    return snprintf(buf, bufsize, "%g", AS_NUMBER(value));
  case VAL_OBJECT:
    return obj_sprintf(buf, bufsize, AS_OBJECT(value));
  }
  assert(false && "unknown value type");
  return 0;
}

uint32_t val_hash(Value value) {
  switch (value.type) {
  case VAL_EMPTY:
    return 0;
  case VAL_NIL:
    return 0;
  case VAL_BOOL:
    return AS_BOOL(value) ? 1 : 0;
  case VAL_NUMBER: {
    union {
      double number;
      uint64_t bits;
    } u = {.number = AS_NUMBER(value)};
    return (uint32_t)(u.bits ^ (u.bits >> 32));
  }
  case VAL_OBJECT: {
    switch (AS_OBJECT(value)->type) {
    case OBJ_STRING:
      return AS_STRING(value)->hash;
    default:
      return (uint32_t)(uintptr_t)AS_OBJECT(value);
    }
  }
  }
  assert(false && "unknown value type");
  return 0;
}