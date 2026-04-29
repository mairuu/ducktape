#pragma once

#include "string_utils.h"
#include "value.h"

typedef enum { OBJ_STRING } ObjectType;

#define OBJ_MARK_FLAG (1 << 0)

#define STR_INTERNED_FLAG (1 << 1)

typedef struct Object {
  ObjectType type;
  int flags;
  struct Object *next;
} Object;

typedef struct {
  Object obj;
  String str;
  uint32_t hash;
} ObjString;

static inline bool value_is_obj_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_obj_type(value, OBJ_STRING)

#define AS_STRING(value) ((ObjString *)AS_OBJECT(value))

int obj_sprintf(char *buf, int bufsize, Object *obj);

bool obj_equal(Object *a, Object *b);

void obj_free(Object **obj, Allocator *al);

// -----------------------------------------------------------------------------

ObjString // move ownership of str to the object
    *obj_string_new(String str, uint32_t hash, Allocator *al);

void obj_string_free(ObjString **str, Allocator *al);

void obj_string_set_interned(ObjString *str, bool interned);

bool obj_string_is_interned(ObjString *str);