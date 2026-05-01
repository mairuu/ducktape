#pragma once

#include "allocator.h"
#include "chunk.h"
#include "hash_table.h"
#include "string_utils.h"
#include "value.h"
#include <stdint.h>

typedef enum {
  OBJ_STRING,
  OBJ_FUNCTION,
  OBJ_NATIVE,
  OBJ_UPVALUE,
  OBJ_CLOSURE,
  OBJ_STRUCT_DEF,
  OBJ_STRUCT_INST
} ObjectType;

#define OBJ_MARKED_FLAG (1u << 0)   // bit 0
#define STR_INTERNED_FLAG (1u << 1) // bit 1

#define OBJ_DEF_ID_SHIFT 2
#define OBJ_DEF_ID_MASK (0xFFFFFFFCu) // bits 2–31

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

#define PIC_CACHE_SIZE 4

typedef struct {
  uint16_t shape_id;
  uint16_t offset;
} PicEntry;

typedef struct {
  int name;
  uint8_t count;
  PicEntry entries[PIC_CACHE_SIZE];
} Pic;

typedef struct {
  Object obj;
  int arity;
  int upvalue_count;
  Chunk chunk;
  ObjString *name;
  Pic pic[]; // length is arr_count(chunk->pic)
} ObjFunction;

typedef struct ObjUpvalue {
  Object obj;
  struct ObjUpvalue *next;
  Value *location;
  Value closed;
} ObjUpvalue;

typedef struct {
  Object obj;
  ObjFunction *func;
  ObjUpvalue *upvalues[];
} ObjClosure;

typedef struct VirtualMachine VirtualMachine;

typedef Value (*NativeFunc)(VirtualMachine *vm, int arg_count, Value *args);

typedef struct {
  Object obj;
  NativeFunc func;
} ObjNative;

typedef struct {
  Object obj;
  ObjString *name;
  HashTable fields;
} ObjStructDef;

typedef struct {
  Object obj;
  ObjStructDef *def;
  Value payload[]; // length is def->fields.count
} ObjStructInst;

// -----------------------------------------------------------------------------

static inline bool value_is_obj_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_obj_type(value, OBJ_STRING)
#define IS_FUNCTION(value) value_is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value) value_is_obj_type(value, OBJ_NATIVE)
#define IS_STRUCT_INST(value) value_is_obj_type(value, OBJ_STRUCT_INST)

// -----------------------------------------------------------------------------

#define AS_STRING(value) ((ObjString *)AS_OBJECT(value))
#define AS_FUNCTION(value) ((ObjFunction *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjNative *)AS_OBJECT(value))
#define AS_STRUCT_INST(value) ((ObjStructInst *)AS_OBJECT(value))

// -----------------------------------------------------------------------------

const char *obj_type_name(ObjectType type);

void obj_print(Object *obj);

int obj_sprintf(char *buf, int bufsize, Object *obj);

bool obj_equal(Object *a, Object *b);

void obj_free(Object **obj, Allocator *al);

void obj_set_marked(Object *obj, bool marked);

bool obj_is_marked(Object *obj);

// for only definitions
int obj_def_get_id(Object *obj);

// for only definitions
void obj_def_set_id(Object *obj, int id);

// -----------------------------------------------------------------------------

ObjString // move ownership of str to the object
    *obj_string_new(String str, uint32_t hash, Allocator *al);

void obj_string_free(ObjString **str, Allocator *al);

void obj_string_set_interned(ObjString *str, bool interned);

bool obj_string_is_interned(ObjString *str);

// -----------------------------------------------------------------------------

ObjFunction //
    *obj_function_new(Allocator *al, ObjString *name, ConstantLoader *loader, const Proto *proto);

void obj_function_free(ObjFunction **func, Allocator *al);

// -----------------------------------------------------------------------------

ObjNative //
    *obj_native_new(NativeFunc function, Allocator *al);

void obj_native_free(ObjNative **native, Allocator *al);

// -----------------------------------------------------------------------------

ObjUpvalue //
    *obj_upvalue_new(Allocator *al, Value *location);

void obj_upvalue_free(ObjUpvalue **upvalue, Allocator *al);

// -----------------------------------------------------------------------------

ObjClosure //
    *obj_closure_new(ObjFunction *func, Allocator *al);

void obj_closure_free(ObjClosure **closure, Allocator *al);

// -----------------------------------------------------------------------------

ObjStructDef //
    *obj_struct_def_new(ObjString *name, Allocator *al);

void obj_struct_def_free(ObjStructDef **def, Allocator *al);

// -----------------------------------------------------------------------------

ObjStructInst //
    *obj_struct_inst_new(ObjStructDef *def, Allocator *al);

void obj_struct_inst_free(ObjStructInst **inst, Allocator *al);

// -----------------------------------------------------------------------------