#include "object.h"
#include "ast.h"
#include "chunk.h"
#include "module.h"

#include <assert.h>
#include <string.h>

#define HEAP_FIRST_GC (1u << 20) // 1 MiB
#define TOMBSTONE ((ObjString *)1)

// ── allocation ───────────────────────────────────────────────────────────────

// every heap byte goes through these two so `bytes_allocated` stays honest.
static void *heap_alloc(Heap *h, size_t size) {
  if (h->mark_roots != NULL &&
      (h->stress || h->bytes_allocated + size > h->next_gc)) {
    heap_collect(h);
  }
  h->bytes_allocated += size;
  return al_alloc(&h->al, size);
}

static void heap_dealloc(Heap *h, void *ptr, size_t size) {
  h->bytes_allocated -= size;
  al_free(&h->al, ptr, size);
}

static size_t obj_size(Obj *o) {
  switch (o->kind) {
  case OBJ_STRING:
    return sizeof(ObjString) + (size_t)((ObjString *)o)->len + 1;
  case OBJ_ARRAY:
    return sizeof(ObjArray);
  case OBJ_TUPLE:
    return sizeof(ObjTuple);
  case OBJ_STRUCT:
    return sizeof(ObjStruct);
  case OBJ_ENUM:
    return sizeof(ObjEnum);
  case OBJ_CLOSURE:
    return sizeof(ObjClosure);
  case OBJ_UPVALUE:
    return sizeof(ObjUpvalue);
  }
  assert(false && "unreachable");
  return 0;
}

static void free_obj(Heap *h, Obj *o) {
  switch (o->kind) {
  case OBJ_ARRAY: {
    ObjArray *arr = (ObjArray *)o;
    if (arr->items != NULL) {
      heap_dealloc(h, arr->items, sizeof(Value) * (size_t)arr->count);
    }
    break;
  }
  case OBJ_TUPLE: {
    ObjTuple *tup = (ObjTuple *)o;
    if (tup->items != NULL) {
      heap_dealloc(h, tup->items, sizeof(Value) * (size_t)tup->count);
    }
    break;
  }
  case OBJ_STRUCT: {
    ObjStruct *s = (ObjStruct *)o;
    if (s->def->field_count > 0) {
      heap_dealloc(h, s->fields, sizeof(Value) * (size_t)s->def->field_count);
    }
    break;
  }
  case OBJ_ENUM: {
    ObjEnum *e = (ObjEnum *)o;
    if (e->variant->field_count > 0) {
      heap_dealloc(h, e->fields,
                   sizeof(Value) * (size_t)e->variant->field_count);
    }
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *c = (ObjClosure *)o;
    if (c->upvalue_count > 0) {
      heap_dealloc(h, c->upvalues,
                   sizeof(ObjUpvalue *) * (size_t)c->upvalue_count);
    }
    break;
  }
  default:
    break;
  }
  heap_dealloc(h, o, obj_size(o));
}

static Obj *new_obj(Heap *h, ObjKind kind, size_t size) {
  Obj *o = heap_alloc(h, size);
  o->kind = kind;
  o->marked = false;
  o->next = h->objects;
  h->objects = o;
  return o;
}

// ── intern table (open addressing, linear probing, tombstones) ───────────────

static uint32_t hash_chars(const char *chars, int len) {
  uint32_t hash = 2166136261u; // FNV-1a
  for (int i = 0; i < len; i++) {
    hash ^= (uint8_t)chars[i];
    hash *= 16777619;
  }
  return hash;
}

static void table_insert(Heap *h, ObjString *s) {
  uint32_t i = s->hash & (uint32_t)(h->string_cap - 1);
  for (;;) {
    ObjString *slot = h->strings[i];
    if (slot == NULL || slot == TOMBSTONE) {
      h->strings[i] = s;
      h->string_count++;
      return;
    }
    i = (i + 1) & (uint32_t)(h->string_cap - 1);
  }
}

static void table_grow(Heap *h) {
  ObjString **old = h->strings;
  int old_cap = h->string_cap;

  h->string_cap = old_cap < 64 ? 64 : old_cap * 2;
  h->strings =
      al_alloc_zero(&h->al, sizeof(ObjString *) * (size_t)h->string_cap);
  h->string_count = 0; // re-inserting drops the tombstones

  for (int i = 0; i < old_cap; i++) {
    if (old[i] != NULL && old[i] != TOMBSTONE) {
      table_insert(h, old[i]);
    }
  }
  if (old != NULL) {
    al_free(&h->al, old, sizeof(ObjString *) * (size_t)old_cap);
  }
}

static ObjString *table_find(Heap *h, const char *chars, int len,
                             uint32_t hash) {
  if (h->string_cap == 0) {
    return NULL;
  }
  uint32_t i = hash & (uint32_t)(h->string_cap - 1);
  for (;;) {
    ObjString *slot = h->strings[i];
    if (slot == NULL) {
      return NULL;
    }
    if (slot != TOMBSTONE && slot->len == len && slot->hash == hash &&
        memcmp(slot->chars, chars, (size_t)len) == 0) {
      return slot;
    }
    i = (i + 1) & (uint32_t)(h->string_cap - 1);
  }
}

// ── constructors ─────────────────────────────────────────────────────────────

ObjString *heap_intern(Heap *h, const char *chars, int len) {
  uint32_t hash = hash_chars(chars, len);
  ObjString *found = table_find(h, chars, len, hash);
  if (found != NULL) {
    return found;
  }

  // `chars` is never GC memory, so collecting inside new_obj is safe.
  ObjString *s =
      (ObjString *)new_obj(h, OBJ_STRING, sizeof(ObjString) + (size_t)len + 1);
  s->len = len;
  s->hash = hash;
  memcpy(s->chars, chars, (size_t)len);
  s->chars[len] = '\0';

  if (h->string_count + 1 > h->string_cap * 3 / 4) {
    table_grow(h);
  }
  table_insert(h, s);
  return s;
}

ObjString *heap_concat(Heap *h, ObjString *a, ObjString *b) {
  int len = a->len + b->len;
  char *buf = al_alloc(&h->al, (size_t)len + 1);
  memcpy(buf, a->chars, (size_t)a->len);
  memcpy(buf + a->len, b->chars, (size_t)b->len);
  ObjString *s = heap_intern(h, buf, len);
  al_free(&h->al, buf, (size_t)len + 1);
  return s;
}

ObjArray *heap_array(Heap *h, int count) {
  // items first: if allocating the Obj collects, a half-built array is never
  // sitting unreachable on the object list.
  Value *items = NULL;
  if (count > 0) {
    items = heap_alloc(h, sizeof(Value) * (size_t)count);
    for (int i = 0; i < count; i++) {
      items[i] = val_unit();
    }
  }
  ObjArray *arr = (ObjArray *)new_obj(h, OBJ_ARRAY, sizeof(ObjArray));
  arr->count = count;
  arr->items = items;
  return arr;
}

ObjTuple *heap_tuple(Heap *h, int count) {
  Value *items = NULL;
  if (count > 0) {
    items = heap_alloc(h, sizeof(Value) * (size_t)count);
    for (int i = 0; i < count; i++) {
      items[i] = val_unit();
    }
  }
  ObjTuple *tup = (ObjTuple *)new_obj(h, OBJ_TUPLE, sizeof(ObjTuple));
  tup->count = count;
  tup->items = items;
  return tup;
}

ObjStruct *heap_struct(Heap *h, StructDef *def) {
  Value *fields = NULL;
  if (def->field_count > 0) {
    fields = heap_alloc(h, sizeof(Value) * (size_t)def->field_count);
    for (int i = 0; i < def->field_count; i++) {
      fields[i] = val_unit();
    }
  }
  ObjStruct *s = (ObjStruct *)new_obj(h, OBJ_STRUCT, sizeof(ObjStruct));
  s->def = def;
  s->fields = fields;
  return s;
}

ObjEnum *heap_enum(Heap *h, VariantDef *variant) {
  Value *fields = NULL;
  if (variant->field_count > 0) {
    fields = heap_alloc(h, sizeof(Value) * (size_t)variant->field_count);
    for (int i = 0; i < variant->field_count; i++) {
      fields[i] = val_unit();
    }
  }
  ObjEnum *e = (ObjEnum *)new_obj(h, OBJ_ENUM, sizeof(ObjEnum));
  e->variant = variant;
  e->fields = fields;
  return e;
}

ObjClosure *heap_closure(Heap *h, FunDef *fun, int upvalue_count) {
  // allocate the upvalue array first; new_obj may collect, and the array is
  // raw (no live Values yet), so ordering here is safe. the caller fills the
  // slots immediately after, before the next allocation.
  ObjUpvalue **upvalues = NULL;
  if (upvalue_count > 0) {
    upvalues = heap_alloc(h, sizeof(ObjUpvalue *) * (size_t)upvalue_count);
    for (int i = 0; i < upvalue_count; i++) {
      upvalues[i] = NULL;
    }
  }
  ObjClosure *c = (ObjClosure *)new_obj(h, OBJ_CLOSURE, sizeof(ObjClosure));
  c->fun = fun;
  c->upvalues = upvalues;
  c->upvalue_count = upvalue_count;
  return c;
}

ObjUpvalue *heap_upvalue(Heap *h, Value *slot) {
  ObjUpvalue *uv = (ObjUpvalue *)new_obj(h, OBJ_UPVALUE, sizeof(ObjUpvalue));
  uv->location = slot;
  uv->closed = val_unit();
  uv->next = NULL;
  return uv;
}

// ── mark-sweep ───────────────────────────────────────────────────────────────

static void mark_obj(Obj *o) {
  if (o == NULL || o->marked) {
    return;
  }
  o->marked = true;
  switch (o->kind) {
  case OBJ_STRING:
    break;
  case OBJ_ARRAY: {
    ObjArray *arr = (ObjArray *)o;
    for (int i = 0; i < arr->count; i++) {
      gc_mark_value(arr->items[i]);
    }
    break;
  }
  case OBJ_TUPLE: {
    ObjTuple *tup = (ObjTuple *)o;
    for (int i = 0; i < tup->count; i++) {
      gc_mark_value(tup->items[i]);
    }
    break;
  }
  case OBJ_STRUCT: {
    ObjStruct *s = (ObjStruct *)o;
    for (int i = 0; i < s->def->field_count; i++) {
      gc_mark_value(s->fields[i]);
    }
    break;
  }
  case OBJ_ENUM: {
    ObjEnum *e = (ObjEnum *)o;
    for (int i = 0; i < e->variant->field_count; i++) {
      gc_mark_value(e->fields[i]);
    }
    break;
  }
  case OBJ_CLOSURE: {
    ObjClosure *c = (ObjClosure *)o;
    for (int i = 0; i < c->upvalue_count; i++) {
      mark_obj((Obj *)c->upvalues[i]);
    }
    break;
  }
  case OBJ_UPVALUE:
    // `location` points at a live stack slot (already a root) while open, or
    // at `closed` once closed; marking `closed` covers the closed case and is
    // harmless while open (initialised to unit at capture time).
    gc_mark_value(((ObjUpvalue *)o)->closed);
    break;
  }
}

void gc_mark_value(Value v) {
  if (v.kind == VAL_OBJ) {
    mark_obj(v.as.obj);
  }
}

static void mark_fun_consts(FunDef *fun) {
  Chunk *chunk = fun->chunk;
  if (chunk == NULL) {
    return;
  }
  for (int j = 0; j < chunk->const_count; j++) {
    gc_mark_value(chunk->consts[j]);
  }
}

void heap_collect(Heap *h) {
  // roots: every compiled chunk's constants, plus the VM stack if running
  if (h->module != NULL) {
    for (int i = 0; i < h->module->fun_count; i++) {
      mark_fun_consts(h->module->funs[i]);
    }
    for (int i = 0; i < h->module->method_count; i++) {
      mark_fun_consts(h->module->methods[i]);
    }
    for (int i = 0; i < h->module->closure_count; i++) {
      mark_fun_consts(h->module->closures[i]);
    }
  }
  if (h->mark_roots != NULL) {
    h->mark_roots(h->mark_roots_ctx);
  }

  // the intern table is weak: drop entries the mark phase didn't reach.
  // `string_count` counts occupied slots (live + tombstone), not just live
  // entries — tombstoning doesn't decrement it. Otherwise the load-factor
  // check in heap_intern goes blind to tombstone buildup, the table never
  // regrows to purge them, and a lookup that probes a fully-tombstoned
  // table (no NULL slot left to stop at) spins forever.
  for (int i = 0; i < h->string_cap; i++) {
    ObjString *s = h->strings[i];
    if (s != NULL && s != TOMBSTONE && !s->obj.marked) {
      h->strings[i] = TOMBSTONE;
    }
  }

  // sweep: free everything unmarked, clear marks on survivors
  Obj **cur = &h->objects;
  while (*cur != NULL) {
    Obj *o = *cur;
    if (o->marked) {
      o->marked = false;
      cur = &o->next;
    } else {
      *cur = o->next;
      free_obj(h, o);
    }
  }

  h->next_gc = h->bytes_allocated * 2;
  if (h->next_gc < HEAP_FIRST_GC) {
    h->next_gc = HEAP_FIRST_GC;
  }
}

// ── lifecycle ────────────────────────────────────────────────────────────────

void heap_init(Heap *h, Module *module, bool stress) {
  *h = (Heap){
      .al = heap_allocator_create(),
      .next_gc = HEAP_FIRST_GC,
      .stress = stress,
      .module = module,
  };
}

void heap_destroy(Heap *h) {
  Obj *o = h->objects;
  while (o != NULL) {
    Obj *next = o->next;
    free_obj(h, o);
    o = next;
  }
  if (h->strings != NULL) {
    al_free(&h->al, h->strings, sizeof(ObjString *) * (size_t)h->string_cap);
  }
  *h = (Heap){0};
}
