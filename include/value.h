#pragma once

#include <stdint.h>

typedef enum {
  VAL_EMPTY, // internal
  VAL_NIL,
  VAL_BOOL,
  VAL_NUMBER,
  VAL_OBJECT,
} ValueType;

typedef struct Object Object;

typedef struct {
  ValueType type;
  union {
    bool boolean;
    double number;
    Object *object;
  } as;
} Value;

#define IS_EMPTY(value) ((value).type == VAL_EMPTY)
#define IS_NIL(value) ((value).type == VAL_NIL)
#define IS_BOOL(value) ((value).type == VAL_BOOL)
#define IS_NUMBER(value) ((value).type == VAL_NUMBER)
#define IS_OBJECT(value) ((value).type == VAL_OBJECT)

#define AS_NIL(value) (value)
#define AS_BOOL(value) ((value).as.boolean)
#define AS_NUMBER(value) ((value).as.number)
#define AS_OBJECT(value) ((value).as.object)

#define EMPTY_VALUE ((Value){.type = VAL_EMPTY})
#define NIL_VAL ((Value){.type = VAL_NIL})
#define BOOL_VAL(value) ((Value){.type = VAL_BOOL, .as.boolean = (value)})
#define NUMBER_VAL(value) ((Value){.type = VAL_NUMBER, .as.number = (value)})
#define OBJECT_VAL(obj) ((Value){.type = VAL_OBJECT, .as.object = (Object *)(obj)})

bool val_equal(Value a, Value b);

bool val_is_falsey(Value value);

void val_print(Value value);

int val_sprintf(char *buf, int bufsize, Value value);

uint32_t val_hash(Value value);