#include "object.h"
#include "ast.h"
#include "chunk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_FIRST_GC (1u << 20) // 1 MiB
#define TOMBSTONE ((ObjString *)1)

// ── allocation ───────────────────────────────────────────────────────────────

// every heap byte goes through these two so `bytes_allocated` stays honest.
//
// Exhaustion is not a runtime error: there is no unwinding, and reporting one
// from here would have to allocate. What the check buys is that the failure is
// *defined* — every caller below writes into the result without testing it, so
// without this a NULL is a segfault. Milestone 106 is what made it reachable in
// one step, `array::fill` being the first place a program names a size.
static void *heap_alloc(Heap *h, size_t size) {
  if (h->mark_roots != NULL &&
      (h->stress || h->bytes_allocated + size > h->next_gc)) {
    heap_collect(h);
  }
  h->bytes_allocated += size;
  void *ptr = al_alloc(&h->al, size);
  if (ptr == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  return ptr;
}

static void heap_dealloc(Heap *h, void *ptr, size_t size) {
  h->bytes_allocated -= size;
  al_free(&h->al, ptr, size);
}

// bytes for a fixed-arity aggregate holding `count` values inline.
static size_t aggregate_size(size_t base, int count) {
  return base + sizeof(Value) * (size_t)count;
}

static size_t obj_size(Obj *o) {
  switch (o->kind) {
  case OBJ_STRING:
    return sizeof(ObjString) + (size_t)((ObjString *)o)->len + 1;
  case OBJ_STRBUF:
    return sizeof(ObjStrBuf);
  case OBJ_BYTES:
    return sizeof(ObjBytes);
  case OBJ_ARRAY:
    return sizeof(ObjArray);
  // the fixed-arity three carry their values, so their size is their arity —
  // read back from the same place the constructor read it, since that is the
  // only record of how many bytes were asked for.
  case OBJ_TUPLE:
    return aggregate_size(sizeof(ObjTuple), ((ObjTuple *)o)->count);
  case OBJ_STRUCT:
    return aggregate_size(sizeof(ObjStruct),
                          ((ObjStruct *)o)->def->field_count);
  case OBJ_ENUM:
    return aggregate_size(sizeof(ObjEnum),
                          ((ObjEnum *)o)->variant->field_count);
  case OBJ_CLOSURE:
    return sizeof(ObjClosure);
  case OBJ_UPVALUE:
    return sizeof(ObjUpvalue);
  case OBJ_DYN:
    return sizeof(ObjDyn);
  }
  assert(false && "unreachable");
  return 0;
}

static void free_obj(Heap *h, Obj *o) {
  switch (o->kind) {
  case OBJ_STRBUF: {
    ObjStrBuf *buf = (ObjStrBuf *)o;
    if (buf->bytes != NULL) {
      heap_dealloc(h, buf->bytes, (size_t)buf->cap); // `cap`, like an array's
    }
    break;
  }
  case OBJ_BYTES: {
    ObjBytes *b = (ObjBytes *)o;
    if (b->bytes != NULL) {
      heap_dealloc(h, b->bytes, (size_t)b->cap);
    }
    break;
  }
  case OBJ_ARRAY: {
    ObjArray *arr = (ObjArray *)o;
    if (arr->items != NULL) {
      // `cap`, not `count`: past the first push the two differ, and the
      // buffer is the size it was asked for rather than the part in use.
      heap_dealloc(h, arr->items, sizeof(Value) * (size_t)arr->cap);
    }
    break;
  }
  // OBJ_TUPLE, OBJ_STRUCT and OBJ_ENUM have nothing to free here: their values
  // are inside the allocation `obj_size` accounts for below.
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

// A buffer starts with no allocation at all, not an empty one: a builder that
// is never pushed to costs one object and nothing else, the same bargain an
// empty array literal makes.
ObjStrBuf *heap_strbuf(Heap *h) {
  ObjStrBuf *buf = (ObjStrBuf *)new_obj(h, OBJ_STRBUF, sizeof(ObjStrBuf));
  buf->len = 0;
  buf->cap = 0;
  buf->bytes = NULL;
  return buf;
}

ObjBytes *heap_bytes(Heap *h) {
  ObjBytes *b = (ObjBytes *)new_obj(h, OBJ_BYTES, sizeof(ObjBytes));
  b->len = 0;
  b->cap = 0;
  b->bytes = NULL;
  return b;
}

#define STRBUF_MIN_CAP 16

// The growth both byte buffers share, taken on the three fields rather than on
// either object: a StringBuf and a Bytes differ in what may enter them, never
// in how the room is made.
static void buf_reserve(Heap *h, char **bytes, int *cap, int len, int needed) {
  if (needed <= *cap) {
    return;
  }
  int new_cap = *cap < STRBUF_MIN_CAP ? STRBUF_MIN_CAP : *cap;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  // The new buffer is allocated before anything about the old one changes,
  // exactly as for an array — but for a weaker reason, and the difference is
  // worth seeing. An array's tail is *traced*, so a `count` raised past a
  // written slot hands the collector a Value that was never stored. Bytes are
  // payload: the collector never reads them, so all the ordering here protects
  // is what a reader would copy. The owning object still has to be rooted
  // across this call, since the collection it may trigger would otherwise sweep
  // the buffer being grown.
  char *grown = heap_alloc(h, (size_t)new_cap);
  if (len > 0) {
    memcpy(grown, *bytes, (size_t)len);
  }
  if (*bytes != NULL) {
    heap_dealloc(h, *bytes, (size_t)*cap);
  }
  *bytes = grown;
  *cap = new_cap;
}

void heap_strbuf_reserve(Heap *h, ObjStrBuf *buf, int needed) {
  buf_reserve(h, &buf->bytes, &buf->cap, buf->len, needed);
}

void heap_bytes_reserve(Heap *h, ObjBytes *b, int needed) {
  buf_reserve(h, &b->bytes, &b->cap, b->len, needed);
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
  arr->cap = count; // exact fit; growth is `push`'s business alone
  arr->items = items;
  return arr;
}

#define ARRAY_MIN_CAP 8

void heap_array_reserve(Heap *h, ObjArray *arr, int needed) {
  if (needed <= arr->cap) {
    return;
  }
  int cap = arr->cap < ARRAY_MIN_CAP ? ARRAY_MIN_CAP : arr->cap;
  while (cap < needed) {
    cap *= 2;
  }

  // The new buffer is allocated *before* anything about `arr` changes, so the
  // collection heap_alloc may trigger still finds a consistent array: `count`
  // live values in the old `items`. Both halves of that matter — the array is
  // reachable (its owner keeps it rooted), so the collector will walk it, and
  // what it walks has to be the buffer that is actually there.
  Value *items = heap_alloc(h, sizeof(Value) * (size_t)cap);
  if (arr->count > 0) { // an empty array has no buffer at all, not an empty one
    memcpy(items, arr->items, sizeof(Value) * (size_t)arr->count);
  }
  for (int i = arr->count; i < cap; i++) {
    items[i] = val_unit(); // the tail is never marked, but is never garbage
  }
  if (arr->items != NULL) {
    heap_dealloc(h, arr->items, sizeof(Value) * (size_t)arr->cap);
  }
  arr->items = items;
  arr->cap = cap;
}

// The three constructors below hand back *unfilled* values, which is safe only
// because each has one caller (OP_TUPLE/OP_STRUCT/OP_ENUM) that writes every
// slot from the stack before doing anything that could allocate — so no
// collection ever sees one raw. Under DEBUG the slots are poisoned with an
// unmappable pointer instead, which turns a violation of that into a crash in
// the collector rather than a garbage pointer traced in silence. `--gc-stress`
// collects at every allocation, so the suite is what checks it.
static void poison_values(Value *values, int count) {
#ifndef NDEBUG
  for (int i = 0; i < count; i++) {
    values[i] = val_obj((Obj *)(uintptr_t)0xDEAD);
  }
#else
  (void)values, (void)count;
#endif
}

ObjTuple *heap_tuple(Heap *h, int count) {
  ObjTuple *tup = (ObjTuple *)new_obj(h, OBJ_TUPLE,
                                      aggregate_size(sizeof(ObjTuple), count));
  tup->count = count;
  poison_values(tup->items, count);
  return tup;
}

ObjStruct *heap_struct(Heap *h, StructDef *def) {
  if (def->field_count == 0) {
    if (def->singleton == NULL) {
      ObjStruct *s = (ObjStruct *)new_obj(h, OBJ_STRUCT, sizeof(ObjStruct));
      s->def = def;
      // filed only now: new_obj may have collected, and the root walk reading
      // a half-built singleton would be worse than reading none.
      def->singleton = &s->obj;
    }
    return (ObjStruct *)def->singleton;
  }
  ObjStruct *s = (ObjStruct *)new_obj(
      h, OBJ_STRUCT, aggregate_size(sizeof(ObjStruct), def->field_count));
  s->def = def;
  poison_values(s->fields, def->field_count);
  return s;
}

ObjEnum *heap_enum(Heap *h, VariantDef *variant) {
  if (variant->field_count == 0) {
    if (variant->singleton == NULL) {
      ObjEnum *e = (ObjEnum *)new_obj(h, OBJ_ENUM, sizeof(ObjEnum));
      e->variant = variant;
      variant->singleton = &e->obj;
    }
    return (ObjEnum *)variant->singleton;
  }
  ObjEnum *e = (ObjEnum *)new_obj(
      h, OBJ_ENUM, aggregate_size(sizeof(ObjEnum), variant->field_count));
  e->variant = variant;
  poison_values(e->fields, variant->field_count);
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

ObjDyn *heap_dyn(Heap *h, Value inner, VTable *vtable) {
  // new_obj may collect, and `inner` is only reachable from the VM stack until
  // it lands in the object — which is why OP_MAKE_DYN leaves it on the stack
  // until the wrapper exists.
  ObjDyn *d = (ObjDyn *)new_obj(h, OBJ_DYN, sizeof(ObjDyn));
  d->inner = inner;
  d->vtable = vtable;
  return d;
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
  case OBJ_STRBUF:
  case OBJ_BYTES:
    break; // pure payload: no Value inside either one to trace
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
  case OBJ_DYN:
    // the vtable is arena-allocated with the Executable, not on this heap —
    // only the wrapped value needs marking. Its methods' constant pools are
    // already roots, since a vtable can only name definitions that are in
    // `exe->globals`.
    gc_mark_value(((ObjDyn *)o)->inner);
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
  if (h->exe != NULL) {
    for (int i = 0; i < h->exe->global_count; i++) {
      mark_fun_consts(h->exe->globals[i]);
    }
    for (int i = 0; i < h->exe->closure_count; i++) {
      mark_fun_consts(h->exe->closures[i]);
    }
    // fieldless singletons are roots rather than a weak cache: there is one
    // per def the program constructs, so the set is bounded by the source
    // text, and keeping them alive is what makes the next `Option::None` a
    // pointer read. A weak cache would have to be cleared in the sweep, and
    // would hand back the allocation the moment a collection landed between
    // two constructions.
    for (int i = 0; i < h->exe->struct_count; i++) {
      mark_obj(h->exe->structs[i]->singleton);
    }
    for (int i = 0; i < h->exe->enum_count; i++) {
      EnumDef *e = h->exe->enums[i];
      for (int j = 0; j < e->variant_count; j++) {
        mark_obj(e->variants[j].singleton);
      }
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

void heap_init(Heap *h, Executable *exe, bool stress) {
  *h = (Heap){
      .al = heap_allocator_create(),
      .next_gc = HEAP_FIRST_GC,
      .stress = stress,
      .exe = exe,
  };
}

void heap_destroy(Heap *h) {
  // the singletons are about to be freed with everything else, and the defs
  // holding them outlive this heap (they belong to the compiler's arena, or to
  // the image's). Unfile them so a second heap over the same program starts
  // from NULL rather than from dangling pointers.
  if (h->exe != NULL) {
    for (int i = 0; i < h->exe->struct_count; i++) {
      h->exe->structs[i]->singleton = NULL;
    }
    for (int i = 0; i < h->exe->enum_count; i++) {
      EnumDef *e = h->exe->enums[i];
      for (int j = 0; j < e->variant_count; j++) {
        e->variants[j].singleton = NULL;
      }
    }
  }

  Obj *o = h->objects;
  while (o != NULL) {
    Obj *next = o->next;
    free_obj(h, o);
    o = next;
  }
  if (h->strings != NULL) {
    al_free(&h->al, h->strings, sizeof(ObjString *) * (size_t)h->string_cap);
  }
  // every object is gone, so the total must be back to zero — the one check
  // that `obj_size` reports the size each object was allocated with.
  assert(h->bytes_allocated == 0);
  *h = (Heap){0};
}
