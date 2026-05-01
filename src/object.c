#include "object.h"

#include <assert.h>
#include <stdio.h>

#include "allocator.h"
#include "array.h"
#include "chunk.h"
#include "hash_table.h"
#include "string_utils.h"
#include "value.h"

static const char *obj_type_names[] = {
    [OBJ_STRING] = "string",           [OBJ_FUNCTION] = "function", [OBJ_NATIVE] = "native",
    [OBJ_UPVALUE] = "upvalue",         [OBJ_CLOSURE] = "closure",   [OBJ_STRUCT_DEF] = "struct-def",
    [OBJ_STRUCT_INST] = "struct-inst",
};

const char *obj_type_name(ObjectType type) {
  if (type < 0 || type >= sizeof(obj_type_names) / sizeof(obj_type_names[0])) {
    return "unknown";
  }
  return obj_type_names[type];
}

void obj_print(Object *obj) {
  char buf[64];
  obj_sprintf(buf, sizeof(buf), obj);
  printf("%s", buf);
}

int obj_sprintf(char *buf, int bufsize, Object *obj) {
  switch (obj->type) {
  case OBJ_STRING: {
    ObjString *str = (ObjString *)obj;
    return snprintf(buf, bufsize, "%.*s", str->str.length, str->str.chars);
  }
  case OBJ_FUNCTION: {
    ObjFunction *fn = (ObjFunction *)obj;
    if (fn->name) {
      return snprintf(buf, bufsize, "<fn %.*s>", fn->name->str.length, fn->name->str.chars);
    } else {
      return snprintf(buf, bufsize, "<fn @%p>", (void *)fn);
    }
  }
  case OBJ_NATIVE: {
    return snprintf(buf, bufsize, "<native fn>");
  }
  case OBJ_UPVALUE: {
    return snprintf(buf, bufsize, "<upvalue @%p>", (void *)obj);
  }
  case OBJ_CLOSURE: {
    ObjFunction *fn = ((ObjClosure *)obj)->func;
    if (fn->name) {
      return snprintf(buf, bufsize, "<fn %.*s>", fn->name->str.length, fn->name->str.chars);
    } else {
      return snprintf(buf, bufsize, "<fn @%p>", (void *)fn);
    }
  }
  case OBJ_STRUCT_DEF: {
    ObjStructDef *def = (ObjStructDef *)obj;
    return snprintf(buf, bufsize, "<struct %.*s>", def->name->str.length, def->name->str.chars);
  }
  case OBJ_STRUCT_INST: {
    ObjStructInst *inst = (ObjStructInst *)obj;
    return snprintf(buf, bufsize, "<instance of %.*s at @%p>", inst->def->name->str.length,
                    inst->def->name->str.chars, (void *)inst);
  }
  default:
    assert(false && "unknown object type");
    return 0;
  }
}

static bool obj_string_equal(ObjString *a, ObjString *b) {
  if (a->obj.flags.interned && b->obj.flags.interned) {
    return a == b;
  }
  return sv_equal(sv_from_str(a->str), sv_from_str(b->str));
}

bool obj_equal(Object *a, Object *b) {
  if (a->type != b->type) {
    return false;
  }

  switch (a->type) {
  case OBJ_STRING:
    return obj_string_equal((ObjString *)a, (ObjString *)b);
  default:
    assert(false && "unknown object type");
    return false;
  }
}

// -----------------------------------------------------------------------------

static void *obj_new(size_t size, ObjectType type, Allocator *al) {
  Object *obj = al_alloc(al, size);
  obj->type = type;
  obj->flags = (ObjectFlags){0};
  obj->next = NULL;
  return obj;
}

void obj_free(Object **obj, Allocator *al) {
  switch ((*obj)->type) {
  case OBJ_STRING:
    obj_string_free((ObjString **)obj, al);
    break;
  case OBJ_FUNCTION:
    obj_function_free((ObjFunction **)obj, al);
    break;
  case OBJ_NATIVE:
    obj_native_free((ObjNative **)obj, al);
    break;
  case OBJ_UPVALUE:
    obj_upvalue_free((ObjUpvalue **)obj, al);
    break;
  case OBJ_CLOSURE:
    obj_closure_free((ObjClosure **)obj, al);
    break;
  case OBJ_STRUCT_DEF:
    obj_struct_def_free((ObjStructDef **)obj, al);
    break;
  case OBJ_STRUCT_INST:
    obj_struct_inst_free((ObjStructInst **)obj, al);
    break;
  default:
    assert(false && "unknown object type");
    break;
  }
}

// -----------------------------------------------------------------------------

ObjString //
    *obj_string_new(String str, uint32_t hash, Allocator *al) {
  ObjString *obj = obj_new(sizeof(ObjString), OBJ_STRING, al);
  obj->str = str;
  obj->hash = hash;
  return obj;
}

void obj_string_free(ObjString **obj, Allocator *al) {
  if (!*obj) {
    return;
  }
  str_destroy(&(*obj)->str, al);
  al_free(al, *obj, sizeof(ObjString));
  *obj = NULL;
}

// -----------------------------------------------------------------------------

static inline size_t function_size(int pic_count) {
  return sizeof(ObjFunction) + sizeof(Pic) * pic_count;
}

ObjFunction //
    *obj_function_new(Allocator *al, ObjString *name, ConstantLoader *loader, const Proto *proto) {
  int pic_count = arr_count(proto->pic);
  ObjFunction *obj = obj_new(function_size(pic_count), OBJ_FUNCTION, al);

  obj->arity = proto->arity;
  obj->upvalue_count = proto->upvalue_count;
  chunk_load_from_proto(&obj->chunk, proto, loader, al);
  obj->name = name;

  for (int i = 0; i < pic_count; i++) {
    obj->pic[i].name = obj->chunk.pic->data[i];
    obj->pic[i].count = 0;
    for (int j = 0; j < PIC_CACHE_SIZE; j++) {
      obj->pic[i].entries[j].shape_id = 0;
      obj->pic[i].entries[j].offset = 0;
    }
  }

  return obj;
}

void obj_function_free(ObjFunction **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  int pic_count = arr_count((*obj)->chunk.pic);
  chunk_destroy(&(*obj)->chunk, al);
  al_free(al, *obj, function_size(pic_count));
  *obj = NULL;
}

// -----------------------------------------------------------------------------

ObjNative //
    *obj_native_new(NativeFunc function, Allocator *al) {
  ObjNative *obj = obj_new(sizeof(ObjNative), OBJ_NATIVE, al);
  obj->func = function;
  return obj;
}

void obj_native_free(ObjNative **native, Allocator *al) {
  if (!*native) {
    return;
  }
  al_free(al, *native, sizeof(ObjNative));
  *native = NULL;
}

// -----------------------------------------------------------------------------

ObjUpvalue //
    *obj_upvalue_new(Allocator *al, Value *location) {
  ObjUpvalue *obj = obj_new(sizeof(ObjUpvalue), OBJ_UPVALUE, al);
  obj->closed = EMPTY_VAL;
  obj->location = location;
  return obj;
}

void obj_upvalue_free(ObjUpvalue **upvalue, Allocator *al) {
  if (!upvalue) {
    return;
  }
  al_free(al, *upvalue, sizeof(ObjUpvalue));
  *upvalue = NULL;
}

// -----------------------------------------------------------------------------

static inline size_t closure_size(int upvalue_count) {
  return sizeof(ObjClosure) + sizeof(ObjUpvalue *) * upvalue_count;
}

ObjClosure //
    *obj_closure_new(ObjFunction *func, Allocator *al) {
  ObjClosure *obj = obj_new(closure_size(func->upvalue_count), OBJ_CLOSURE, al);
  obj->func = func;
  for (int i = 0; i < func->upvalue_count; i++) {
    obj->upvalues[i] = NULL;
  }
  return obj;
}

void obj_closure_free(ObjClosure **closure, Allocator *al) {
  if (!closure) {
    return;
  }
  al_free(al, *closure, closure_size((*closure)->func->upvalue_count));
  *closure = NULL;
}

// -----------------------------------------------------------------------------

ObjStructDef //
    *obj_struct_def_new(ObjString *name, Allocator *al) {
  ObjStructDef *obj = obj_new(sizeof(ObjStructDef), OBJ_STRUCT_DEF, al);
  obj->name = name;
  ht_init(&obj->fields);
  return obj;
}

void obj_struct_def_free(ObjStructDef **def, Allocator *al) {
  if (!def) {
    return;
  }
  ht_destroy(&(*def)->fields, al);
  al_free(al, *def, sizeof(ObjStructDef));
  *def = NULL;
}

// -----------------------------------------------------------------------------

static inline size_t struct_inst_size(int field_count) {
  return sizeof(ObjStructInst) + sizeof(Value) * field_count;
}

ObjStructInst //
    *obj_struct_inst_new(ObjStructDef *def, Allocator *al) {
  ObjStructInst *obj = obj_new(struct_inst_size(def->fields.count), OBJ_STRUCT_INST, al);
  obj->def = def;
  for (int i = 0; i < def->fields.count; i++) {
    obj->payload[i] = EMPTY_VAL;
  }
  return obj;
}

void obj_struct_inst_free(ObjStructInst **inst, Allocator *al) {
  if (!inst) {
    return;
  }
  al_free(al, *inst, struct_inst_size((*inst)->def->fields.count));
  *inst = NULL;
}