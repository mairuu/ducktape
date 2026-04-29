#pragma once

#include "chunk.h"
#include "string_utils.h"
#include "value.h"

typedef enum { OBJ_STRING, OBJ_FUNCTION, OBJ_NATIVE } ObjectType;

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

typedef struct {
  Object obj;
  int arity;
  Chunk chunk;
  ObjString *name;
} ObjFunction;

typedef struct VirtualMachine VirtualMachine;

typedef Value (*NativeFunc)(VirtualMachine *vm, int arg_count, Value *args);

typedef struct {
  Object obj;
  NativeFunc function;
} ObjNative;

// -----------------------------------------------------------------------------

static inline bool value_is_obj_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_obj_type(value, OBJ_STRING)
#define IS_FUNCTION(value) value_is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value) value_is_obj_type(value, OBJ_NATIVE)

// -----------------------------------------------------------------------------

#define AS_STRING(value) ((ObjString *)AS_OBJECT(value))
#define AS_FUNCTION(value) ((ObjFunction *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjNative *)AS_OBJECT(value))

// -----------------------------------------------------------------------------

const char *obj_type_name(ObjectType type);

int obj_sprintf(char *buf, int bufsize, Object *obj);

bool obj_equal(Object *a, Object *b);

void obj_free(Object **obj, Allocator *al);

// -----------------------------------------------------------------------------

ObjString // move ownership of str to the object
    *obj_string_new(String str, uint32_t hash, Allocator *al);

void obj_string_free(ObjString **str, Allocator *al);

void obj_string_set_interned(ObjString *str, bool interned);

bool obj_string_is_interned(ObjString *str);

// -----------------------------------------------------------------------------

ObjFunction //
    *obj_function_new(Allocator *al, int arity);

void obj_function_free(ObjFunction **func, Allocator *al);

// -----------------------------------------------------------------------------

ObjNative //
    *obj_native_new(NativeFunc function, Allocator *al);

void obj_native_free(ObjNative **native, Allocator *al);