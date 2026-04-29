#include "object.h"

#include <assert.h>
#include <stdio.h>

#include "string_utils.h"

int obj_sprintf(char *buf, int bufsize, Object *obj) {
  switch (obj->type) {
  case OBJ_STRING: {
    ObjString *str = (ObjString *)obj;
    return snprintf(buf, bufsize, "%.*s", str->str.length, str->str.chars);
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
  default:
    assert(false && "unknown object type");
    break;
  }
}

ObjString //
    *obj_string_new(String str, uint32_t hash, Allocator *al) {
  ObjString *obj = obj_new(sizeof(ObjString), OBJ_STRING, al);
  obj->str = str;
  obj->hash = hash;
  return obj;
}

void obj_string_free(ObjString **obj, Allocator *al) {
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