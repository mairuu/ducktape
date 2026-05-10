#pragma once

#include "allocator.h"
#include "array.h"
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
  OBJ_STRUCT_INST,
  OBJ_TRAIT_DEF,
  OBJ_IMPL,
  OBJ_TRAIT_OBJECT,
  OBJ_ARRAY,
  OBJ_MAP,
} ObjectType;

typedef union {
  uint32_t raw;
  struct {
    uint8_t marked : 1;
    uint8_t interned : 1;
    uint8_t _pad : 6;
    uint32_t def_id : 24;
  };
} ObjectFlags;

typedef enum { DEFID_NONE = 0, DEFID_START } ReservedDefID;

typedef struct Object {
  ObjectType type;
  ObjectFlags flags;
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
  IntArray *param_constraints;
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

typedef struct ObjImpl ObjImpl;

typedef struct {
  Object obj;
  ObjString *name;
  ObjImpl *inherent_impl;
  HashTable fields;
} ObjStructDef;

typedef struct {
  Object obj;
  ObjStructDef *def;
  Value payload[]; // length is def->fields.count
} ObjStructInst;

typedef struct {
  Object obj;
  ObjString *name;
  ObjImpl *default_impl; // null if no default impl provided
  int method_count;
} ObjTraitDef;

typedef struct ObjImpl {
  Object obj;
  ObjTraitDef *trait;
  Object *subject;
  Object *impls[]; // length is trait->method_count
} ObjImpl;

typedef struct {
  Object obj;
  Object *receiver;
  ObjImpl *impl;
} ObjTraitObject;

typedef struct {
  Object obj;
  ValueArray *values;
} ObjArray;

typedef struct {
  Object obj;
  HashTable entries;
} ObjMap;

// -----------------------------------------------------------------------------

static inline bool value_is_obj_type(Value value, ObjectType type) {
  return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

#define IS_STRING(value) value_is_obj_type(value, OBJ_STRING)
#define IS_FUNCTION(value) value_is_obj_type(value, OBJ_FUNCTION)
#define IS_UPVALUE(value) value_is_obj_type(value, OBJ_UPVALUE)
#define IS_CLOSURE(value) value_is_obj_type(value, OBJ_CLOSURE)
#define IS_NATIVE(value) value_is_obj_type(value, OBJ_NATIVE)
#define IS_STRUCT_DEF(value) value_is_obj_type(value, OBJ_STRUCT_DEF)
#define IS_STRUCT_INST(value) value_is_obj_type(value, OBJ_STRUCT_INST)
#define IS_TRAIT_DEF(value) value_is_obj_type(value, OBJ_TRAIT_DEF)
#define IS_IMPL(value) value_is_obj_type(value, OBJ_IMPL)
#define IS_TRAIT_OBJECT(value) value_is_obj_type(value, OBJ_TRAIT_OBJECT)
#define IS_ARRAY(value) value_is_obj_type(value, OBJ_ARRAY)
#define IS_MAP(value) value_is_obj_type(value, OBJ_MAP)

// -----------------------------------------------------------------------------

#define AS_STRING(value) ((ObjString *)AS_OBJECT(value))
#define AS_FUNCTION(value) ((ObjFunction *)AS_OBJECT(value))
#define AS_UPVALUE(value) ((ObjUpvalue *)AS_OBJECT(value))
#define AS_CLOSURE(value) ((ObjClosure *)AS_OBJECT(value))
#define AS_NATIVE(value) ((ObjNative *)AS_OBJECT(value))
#define AS_STRUCT_DEF(value) ((ObjStructDef *)AS_OBJECT(value))
#define AS_STRUCT_INST(value) ((ObjStructInst *)AS_OBJECT(value))
#define AS_TRAIT_DEF(value) ((ObjTraitDef *)AS_OBJECT(value))
#define AS_IMPL(value) ((ObjImpl *)AS_OBJECT(value))
#define AS_TRAIT_OBJECT(value) ((ObjTraitObject *)AS_OBJECT(value))
#define AS_ARRAY(value) ((ObjArray *)AS_OBJECT(value))
#define AS_MAP(value) ((ObjMap *)AS_OBJECT(value))

// -----------------------------------------------------------------------------

const char *obj_type_name(ObjectType type);

void obj_print(Object *obj);

int obj_sprintf(char *buf, int bufsize, Object *obj);

bool obj_equal(Object *a, Object *b);

void obj_free(Object **obj, Allocator *al);

// -----------------------------------------------------------------------------

ObjString // move ownership of str to the object
    *obj_string_new(String str, uint32_t hash, Allocator *al);

void obj_string_free(ObjString **str, Allocator *al);

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

ObjTraitDef //
    *obj_trait_def_new(ObjString *name, int method_count, Allocator *al);

void obj_trait_def_free(ObjTraitDef **def, Allocator *al);

// -----------------------------------------------------------------------------

ObjImpl //
    *obj_impl_new(ObjTraitDef *trait, Object *subject, Allocator *al);

void obj_impl_free(ObjImpl **impl, Allocator *al);

// -----------------------------------------------------------------------------

ObjTraitObject //
    *obj_trait_object_new(Object *receiver, ObjImpl *impl, Allocator *al);

void obj_trait_object_free(ObjTraitObject **trait_obj, Allocator *al);

// -----------------------------------------------------------------------------

ObjArray //
    *obj_array_new(int initial_capacity, Allocator *al);

void obj_array_free(ObjArray **array, Allocator *al);

// -----------------------------------------------------------------------------

ObjMap //
    *obj_map_new(Allocator *al);

void obj_map_free(ObjMap **map, Allocator *al);