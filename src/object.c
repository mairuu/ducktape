#include "object.h"

#include <assert.h>
#include <stdio.h>

#include "string_utils.h"

static const char *obj_type_names[] = {
    [OBJ_STRING] = "string",
    [OBJ_FUNCTION] = "function",
    [OBJ_NATIVE] = "native",
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
  default:
    assert(false && "unknown object type");
    return 0;
  }
}

static bool obj_string_equal(ObjString *a, ObjString *b) {
  if (obj_string_is_interned(a) && obj_string_is_interned(b)) {
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
  obj->flags = 0;
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
  default:
    assert(false && "unknown object type");
    break;
  }
}

void obj_set_marked(Object *obj, bool marked) {
  if (marked) {
    obj->flags |= OBJ_MARKED_FLAG;
  } else {
    obj->flags &= ~OBJ_MARKED_FLAG;
  }
}

bool obj_is_marked(Object *obj) {
  return obj->flags & OBJ_MARKED_FLAG;
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

void obj_string_set_interned(ObjString *str, bool interned) {
  if (interned) {
    str->obj.flags |= STR_INTERNED_FLAG;
  } else {
    str->obj.flags &= ~STR_INTERNED_FLAG;
  }
}

bool obj_string_is_interned(ObjString *str) { return str->obj.flags & STR_INTERNED_FLAG; }

// -----------------------------------------------------------------------------

ObjFunction //
    *obj_function_new(Allocator *al, int arity) {
  ObjFunction *obj = obj_new(sizeof(ObjFunction), OBJ_FUNCTION, al);
  obj->arity = arity;
  chunk_init(&obj->chunk);
  obj->name = NULL;
  return obj;
}

void obj_function_free(ObjFunction **obj, Allocator *al) {
  if (!*obj) {
    return;
  }

  chunk_destroy(&(*obj)->chunk, al);
  al_free(al, *obj, sizeof(ObjFunction));
  *obj = NULL;
}

// -----------------------------------------------------------------------------

ObjNative //
    *obj_native_new(NativeFunc function, Allocator *al) {
  ObjNative *obj = obj_new(sizeof(ObjNative), OBJ_NATIVE, al);
  obj->function = function;
  return obj;
}

void obj_native_free(ObjNative **native, Allocator *al) {
  if (!*native) {
    return;
  }
  al_free(al, *native, sizeof(ObjNative));
  *native = NULL;
}