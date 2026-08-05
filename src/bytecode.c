#include "bytecode.h"
#include "chunk.h"
#include "string_utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Image format
// ═══════════════════════════════════════════════════════════════════════════════
//
//   header   "DTBC", u16 version, u16 entry (a globals index)
//   counts   u32 strings, globals, structs, enums, closures, vtables
//   strings  [u32 len, len bytes]*        — every name and string constant
//   structs  [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]
//   enums    [u32 name, u16 variant_count,
//               [u32 name, u8 is_tuple, u16 field_count, u32 field_name*]*]
//   vtables  [u16 method_count, u32 fun_index*]*   (VTABLE_SLOT_EMPTY == none)
//   upcasts  [u16 link_count, [u16 pair, u32 vtable_index]*]*  — one record
//              per vtable, in the same order, so a link may name a table
//              written after its own
//   funs     [u32 name, u16 param_count, u8 body_kind,
//               body_kind == BC_BODY_CHUNK:  u32 code_len, code_len bytes,
//                                            u16 const_count, const*
//               body_kind == BC_BODY_NATIVE: u32 registry name]*
//                                            — globals first, then closures
//
// A native is written by *name*: a C function pointer is not a thing an image
// can carry, and an image is the runtime projection of the program with no
// compiler behind it. `bc_load` re-binds each name against the running
// binary's registry, so a mismatched native ABI is a clean error at load
// rather than a wrong call later. An @intrinsic never appears at all — it is
// an opcode, lowered inline, with no definition to write.
//
// A vtable is written as bare function indices: which trait and which self
// type it was built for is compile-time bookkeeping the VM never consults, so
// it is not in the image — the same rule that keeps types and spans out. Its
// upcast links follow the same rule and survive it: a link is two tables and
// an opaque pair id, none of which is a type.
//
// A constant is a u8 tag and its payload; the pointer-shaped ones become
// indices (BC_C_STR into the string table, BC_C_FUN into the funs section,
// counted globals-then-closures) so the pool holds no raw addresses. That is
// what keeps both directions a straight loop.
//
// Struct/enum slots need no such treatment: OP_STRUCT/OP_ENUM already carry a
// slot the VM resolves through `exe`, and the tables are written in slot order.

// A vtable slot with no target: the trait excluded an undispatchable provided
// method, so no function fills it. Out of range for any real globals index
// (which the loader bounds-checks against `global_count` <= BC_MAX_SLOTS).
#define VTABLE_SLOT_EMPTY 0xFFFFFFFFu

// what a fun record carries in place of a body.
typedef enum {
  BC_BODY_NONE,   // no body: calling it is the VM's existing runtime error
  BC_BODY_CHUNK,  // compiled ducktape
  BC_BODY_NATIVE, // a registry name to re-bind at load
} BcBodyKind;

typedef enum {
  BC_C_INT,
  BC_C_FLOAT,
  BC_C_BOOL,
  BC_C_UNIT,
  BC_C_RANGE,
  BC_C_FUN,
  BC_C_STR,
  BC_C_CHAR,
} BcConstTag;

// a tuple field has an index where a named one has a name; no string.
#define BC_NO_STRING 0xFFFFFFFFu

static bool bc_error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fprintf(stderr, "error: ");
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  va_end(args);
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Writing
// ═══════════════════════════════════════════════════════════════════════════════

// dedup table for every string the image mentions. Linear probing over an
// array: a program's names and literals number in the hundreds, and this runs
// once per emitted image.
typedef struct {
  StringView *items;
  int count, cap;
  Allocator *al;
} StrTab;

static uint32_t strtab_add(StrTab *t, StringView s) {
  for (int i = 0; i < t->count; i++) {
    if (sv_equal(t->items[i], s)) {
      return (uint32_t)i;
    }
  }
  if (t->count >= t->cap) {
    int new_cap = t->cap == 0 ? 64 : t->cap * 2;
    t->items = al_realloc(t->al, t->items, sizeof(StringView) * (size_t)t->cap,
                          sizeof(StringView) * (size_t)new_cap);
    t->cap = new_cap;
  }
  t->items[t->count] = s;
  return (uint32_t)t->count++;
}

typedef struct {
  FILE *out;
  const Executable *exe;
  StrTab strings;
  bool ok;
} BcWriter;

static void w_bytes(BcWriter *w, const void *src, size_t len) {
  if (w->ok && len > 0 && fwrite(src, 1, len, w->out) != len) {
    w->ok = bc_error("could not write bytecode image");
  }
}

static void w_u8(BcWriter *w, uint8_t v) { w_bytes(w, &v, 1); }

static void w_u16(BcWriter *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  w_bytes(w, b, sizeof b);
}

static void w_u32(BcWriter *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  w_bytes(w, b, sizeof b);
}

static void w_u64(BcWriter *w, uint64_t v) {
  w_u32(w, (uint32_t)v);
  w_u32(w, (uint32_t)(v >> 32));
}

// doubles go out as their IEEE-754 bit pattern rather than a decimal string:
// exact, and the same width on every host that runs this compiler.
static void w_f64(BcWriter *w, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  w_u64(w, bits);
}

static void w_str(BcWriter *w, StringView s) {
  w_u32(w, strtab_add(&w->strings, s));
}

// the index a VAL_FUN constant is written as: `slot` for anything in the
// globals table, otherwise a position in `closures` (nested closure functions
// are reachable only through a constant, so they carry no slot of their own).
static int fun_index(const Executable *exe, const FunDef *fun) {
  if (fun->slot >= 0 && fun->slot < exe->global_count &&
      exe->globals[fun->slot] == fun) {
    return fun->slot;
  }
  for (int i = 0; i < exe->closure_count; i++) {
    if (exe->closures[i] == fun) {
      return exe->global_count + i;
    }
  }
  return -1;
}

// an upcast link names its target table by position. Tables carry no slot of
// their own, so this is a scan — over a list one entry long in most programs
// and only ever walked once per link.
static int vtable_index(const Executable *exe, const VTable *vt) {
  for (int i = 0; i < exe->vtable_count; i++) {
    if (exe->vtables[i] == vt) {
      return i;
    }
  }
  return -1;
}

static void w_const(BcWriter *w, Value v) {
  switch (v.kind) {
  case VAL_INT:
    w_u8(w, BC_C_INT);
    w_u64(w, (uint64_t)v.as.i);
    break;
  case VAL_FLOAT:
    w_u8(w, BC_C_FLOAT);
    w_f64(w, v.as.f);
    break;
  case VAL_BOOL:
    w_u8(w, BC_C_BOOL);
    w_u8(w, v.as.b ? 1 : 0);
    break;
  case VAL_CHAR:
    w_u8(w, BC_C_CHAR);
    w_u32(w, v.as.c);
    break;
  case VAL_UNIT:
    w_u8(w, BC_C_UNIT);
    break;
  case VAL_RANGE:
    w_u8(w, BC_C_RANGE);
    w_u64(w, (uint64_t)v.as.range.start);
    w_u64(w, (uint64_t)v.as.range.end);
    w_u8(w, v.as.range.inclusive ? 1 : 0);
    break;
  case VAL_FUN: {
    int idx = fun_index(w->exe, v.as.fun);
    if (idx < 0) {
      w->ok = bc_error("unlinked function " SV_FMT " in a constant pool",
                       SV_ARG(v.as.fun->name));
      return;
    }
    w_u8(w, BC_C_FUN);
    w_u32(w, (uint32_t)idx);
    break;
  }
  case VAL_OBJ:
    // only strings ever reach a constant pool: every other object is built by
    // an opcode at runtime.
    if (!val_is_string(v)) {
      w->ok = bc_error("non-string heap object in a constant pool");
      return;
    }
    w_u8(w, BC_C_STR);
    w_str(w, (StringView){.chars = val_as_string(v)->chars,
                          .len = val_as_string(v)->len});
    break;
  }
}

static void w_fields(BcWriter *w, const FieldDef *fields, int count,
                     bool is_tuple) {
  w_u8(w, is_tuple ? 1 : 0);
  w_u16(w, (uint16_t)count);
  for (int i = 0; i < count; i++) {
    if (is_tuple) {
      w_u32(w, BC_NO_STRING); // `ident` is a positional index, not a name
    } else {
      w_str(w, fields[i].ident.name);
    }
  }
}

static void w_fun(BcWriter *w, const FunDef *fun) {
  w_str(w, fun->name);
  w_u16(w, (uint16_t)fun->param_count);

  if (fun->native_kind == ATTR_NATIVE) {
    w_u8(w, BC_BODY_NATIVE);
    w_str(w, fun->native_name);
    return;
  }

  const Chunk *chunk = fun->chunk;
  w_u8(w, chunk != NULL ? BC_BODY_CHUNK : BC_BODY_NONE);
  if (chunk == NULL) {
    return;
  }
  w_u32(w, (uint32_t)chunk->count);
  w_bytes(w, chunk->code, (size_t)chunk->count);
  w_u32(w, (uint32_t)chunk->const_count);
  for (int i = 0; i < chunk->const_count; i++) {
    w_const(w, chunk->consts[i]);
  }
}

// walking the program twice is what lets the string table precede the records
// that index into it: this pass only interns, the write pass then only ever
// looks entries up (asserted after the fact — a miss here would mean the two
// walks disagree about which strings an image mentions).
static void collect_strings(BcWriter *w) {
  for (int i = 0; i < w->exe->struct_count; i++) {
    const StructDef *s = w->exe->structs[i];
    strtab_add(&w->strings, s->name);
    if (!s->is_tuple) {
      for (int j = 0; j < s->field_count; j++) {
        strtab_add(&w->strings, s->fields[j].ident.name);
      }
    }
  }
  for (int i = 0; i < w->exe->enum_count; i++) {
    const EnumDef *e = w->exe->enums[i];
    strtab_add(&w->strings, e->name);
    for (int j = 0; j < e->variant_count; j++) {
      const VariantDef *v = &e->variants[j];
      strtab_add(&w->strings, v->name);
      if (!v->is_tuple) {
        for (int k = 0; k < v->field_count; k++) {
          strtab_add(&w->strings, v->fields[k].ident.name);
        }
      }
    }
  }
  for (int i = 0; i < w->exe->global_count + w->exe->closure_count; i++) {
    const FunDef *fun = i < w->exe->global_count
                            ? w->exe->globals[i]
                            : w->exe->closures[i - w->exe->global_count];
    strtab_add(&w->strings, fun->name);
    if (fun->native_kind == ATTR_NATIVE) {
      strtab_add(&w->strings, fun->native_name);
      continue;
    }
    if (fun->chunk == NULL) {
      continue;
    }
    for (int j = 0; j < fun->chunk->const_count; j++) {
      Value v = fun->chunk->consts[j];
      if (val_is_string(v)) {
        strtab_add(&w->strings, (StringView){.chars = val_as_string(v)->chars,
                                             .len = val_as_string(v)->len});
      }
    }
  }
}

bool bc_write(const Executable *exe, const FunDef *entry, const char *path,
              Allocator *al) {
  int entry_idx = fun_index(exe, entry);
  if (entry_idx < 0 || entry_idx >= exe->global_count) {
    return bc_error("entry point " SV_FMT " is not a linked global",
                    SV_ARG(entry->name));
  }

  BcWriter w = {.exe = exe, .strings = {.al = al}, .ok = true};
  collect_strings(&w);

  w.out = fopen(path, "wb");
  if (w.out == NULL) {
    return bc_error("could not open '%s' for writing", path);
  }

  w_bytes(&w, BC_MAGIC, 4);
  w_u16(&w, BC_VERSION);
  w_u32(&w, (uint32_t)entry_idx);

  w_u32(&w, (uint32_t)w.strings.count);
  w_u32(&w, (uint32_t)exe->global_count);
  w_u32(&w, (uint32_t)exe->struct_count);
  w_u32(&w, (uint32_t)exe->enum_count);
  w_u32(&w, (uint32_t)exe->closure_count);
  w_u32(&w, (uint32_t)exe->vtable_count);

  // the table is complete before anything indexes it, so w_str can only ever
  // hit an existing entry from here on.
  int string_count = w.strings.count;
  for (int i = 0; i < string_count; i++) {
    w_u32(&w, (uint32_t)w.strings.items[i].len);
    w_bytes(&w, w.strings.items[i].chars, (size_t)w.strings.items[i].len);
  }

  for (int i = 0; i < exe->struct_count; i++) {
    const StructDef *s = exe->structs[i];
    w_str(&w, s->name);
    w_fields(&w, s->fields, s->field_count, s->is_tuple);
  }
  for (int i = 0; i < exe->enum_count; i++) {
    const EnumDef *e = exe->enums[i];
    w_str(&w, e->name);
    w_u16(&w, (uint16_t)e->variant_count);
    for (int j = 0; j < e->variant_count; j++) {
      const VariantDef *v = &e->variants[j];
      w_str(&w, v->name);
      w_fields(&w, v->fields, v->field_count, v->is_tuple);
    }
  }
  for (int i = 0; i < exe->vtable_count; i++) {
    const VTable *vt = exe->vtables[i];
    w_u16(&w, (uint16_t)vt->method_count);
    for (int j = 0; j < vt->method_count; j++) {
      // an excluded method (undispatchable, so never called through the `dyn`)
      // has no target; a sentinel round-trips the empty slot.
      if (vt->methods[j] == NULL) {
        w_u32(&w, VTABLE_SLOT_EMPTY);
        continue;
      }
      int idx = fun_index(exe, vt->methods[j]);
      if (idx < 0) {
        w.ok = bc_error("internal: vtable method " SV_FMT " is not linked",
                        SV_ARG(vt->methods[j]->name));
        idx = 0;
      }
      w_u32(&w, (uint32_t)idx);
    }
  }
  // the links come after every table, because one may point forward.
  for (int i = 0; i < exe->vtable_count; i++) {
    const VTable *vt = exe->vtables[i];
    w_u16(&w, (uint16_t)vt->upcast_count);
    for (int j = 0; j < vt->upcast_count; j++) {
      int idx = vtable_index(exe, vt->upcasts[j].to);
      if (idx < 0) {
        w.ok = bc_error("internal: upcast target vtable is not linked");
        idx = 0;
      }
      w_u16(&w, vt->upcasts[j].pair);
      w_u32(&w, (uint32_t)idx);
    }
  }
  for (int i = 0; i < exe->global_count; i++) {
    w_fun(&w, exe->globals[i]);
  }
  for (int i = 0; i < exe->closure_count; i++) {
    w_fun(&w, exe->closures[i]);
  }

  if (fclose(w.out) != 0 && w.ok) {
    w.ok = bc_error("could not write bytecode image");
  }
  if (w.ok && w.strings.count != string_count) {
    w.ok = bc_error("internal: string table grew while writing the image");
  }
  if (!w.ok) {
    // a partial image is a valid-looking one: it starts with the magic, so
    // `--run` would try to execute it. Leave nothing behind.
    remove(path);
  }
  return w.ok;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Reading
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
  const uint8_t *bytes;
  size_t pos, len;
  Allocator *al;
  Heap *heap;
  Executable *exe;

  StringView *strings;
  int string_count;

  bool ok;
} BcReader;

// every read goes through here, so a truncated image is caught at the first
// field that runs off the end rather than by whatever it decodes into.
static bool r_take(BcReader *r, size_t n, const uint8_t **out) {
  if (!r->ok) {
    return false;
  }
  if (n > r->len - r->pos) {
    r->ok = bc_error("truncated bytecode image");
    return false;
  }
  *out = r->bytes + r->pos;
  r->pos += n;
  return true;
}

static uint8_t r_u8(BcReader *r) {
  const uint8_t *p;
  return r_take(r, 1, &p) ? *p : 0;
}

static uint16_t r_u16(BcReader *r) {
  const uint8_t *p;
  if (!r_take(r, 2, &p)) {
    return 0;
  }
  return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t r_u32(BcReader *r) {
  const uint8_t *p;
  if (!r_take(r, 4, &p)) {
    return 0;
  }
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint64_t r_u64(BcReader *r) {
  uint64_t lo = r_u32(r);
  return lo | (uint64_t)r_u32(r) << 32;
}

static double r_f64(BcReader *r) {
  uint64_t bits = r_u64(r);
  double v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

static StringView r_str(BcReader *r) {
  uint32_t idx = r_u32(r);
  if (r->ok && idx >= (uint32_t)r->string_count) {
    r->ok = bc_error("bytecode image: string index %u out of range", idx);
  }
  return r->ok ? r->strings[idx] : (StringView){0};
}

static Value r_const(BcReader *r) {
  uint8_t tag = r_u8(r);
  switch (tag) {
  case BC_C_INT:
    return val_int((int64_t)r_u64(r));
  case BC_C_FLOAT:
    return val_float(r_f64(r));
  case BC_C_BOOL:
    return val_bool(r_u8(r) != 0);
  case BC_C_CHAR: {
    // validated rather than trusted: every other tag's payload is total over
    // its bits, and this one is not — a char is a scalar value everywhere
    // else in the runtime, so an image may not be the one place it is not.
    uint32_t cp = r_u32(r);
    if (r->ok && !utf8_is_scalar(cp)) {
      r->ok =
          bc_error("bytecode image: U+%04X is not a Unicode scalar value", cp);
      return val_unit();
    }
    return val_char(cp);
  }
  case BC_C_UNIT:
    return val_unit();
  case BC_C_RANGE: {
    Value v = {.kind = VAL_RANGE};
    v.as.range.start = (int64_t)r_u64(r);
    v.as.range.end = (int64_t)r_u64(r);
    v.as.range.inclusive = r_u8(r) != 0;
    return v;
  }
  case BC_C_FUN: {
    uint32_t idx = r_u32(r);
    int total = r->exe->global_count + r->exe->closure_count;
    if (r->ok && idx >= (uint32_t)total) {
      r->ok = bc_error("bytecode image: function index %u out of range", idx);
      return val_unit();
    }
    // every FunDef exists before any chunk is read, so a forward reference
    // (a global closing over a closure defined later in the file) resolves
    // without a patch-up pass.
    return r->ok ? val_fun(idx < (uint32_t)r->exe->global_count
                               ? r->exe->globals[idx]
                               : r->exe->closures[idx - r->exe->global_count])
                 : val_unit();
  }
  case BC_C_STR: {
    StringView s = r_str(r);
    return r->ok ? val_obj((Obj *)heap_intern(r->heap, s.chars, s.len))
                 : val_unit();
  }
  default:
    if (r->ok) {
      r->ok = bc_error("bytecode image: unknown constant tag %u", tag);
    }
    return val_unit();
  }
}

static FieldDef *r_fields(BcReader *r, int *count_out, bool *is_tuple_out) {
  bool is_tuple = r_u8(r) != 0;
  int count = r_u16(r);
  *is_tuple_out = is_tuple;
  *count_out = count;
  if (count == 0) {
    return NULL;
  }

  FieldDef *fields = al_alloc_zero(r->al, sizeof(FieldDef) * (size_t)count);
  for (int i = 0; i < count; i++) {
    uint32_t idx = r_u32(r);
    if (idx == BC_NO_STRING) {
      fields[i].ident.index = i;
    } else if (idx < (uint32_t)r->string_count) {
      fields[i].ident.name = r->strings[idx];
    } else if (r->ok) {
      r->ok = bc_error("bytecode image: field name index %u out of range", idx);
    }
  }
  return fields;
}

static void r_fun(BcReader *r, FunDef *fun) {
  fun->name = r_str(r);
  fun->param_count = r_u16(r);

  BcBodyKind body_kind = (BcBodyKind)r_u8(r);
  if (body_kind == BC_BODY_NONE) {
    return; // calling it is the VM's existing runtime error
  }
  if (body_kind == BC_BODY_NATIVE) {
    // re-bind against *this* binary's registry. A name it no longer knows is
    // an image built against a different native ABI, which is worth refusing
    // at load rather than discovering at the first call.
    StringView name = r_str(r);
    NativeFn fn = native_lookup(name);
    if (fn == NULL) {
      r->ok = bc_error("image needs a native named '" SV_FMT
                       "', which this build does not provide",
                       SV_ARG(name));
      return;
    }
    fun->native_kind = ATTR_NATIVE;
    fun->native = fn;
    fun->native_name = name;
    return;
  }
  if (body_kind != BC_BODY_CHUNK) {
    r->ok = bc_error("malformed image: unknown function body kind %d",
                     (int)body_kind);
    return;
  }

  Chunk *chunk = al_alloc_zero_for(r->al, Chunk);
  chunk_init(chunk, r->al);

  uint32_t code_len = r_u32(r);
  const uint8_t *code;
  if (!r_take(r, code_len, &code)) {
    return;
  }
  chunk->code = al_alloc(r->al, code_len);
  memcpy(chunk->code, code, code_len);
  chunk->count = chunk->cap = (int)code_len;

  int const_count = (int)r_u32(r);
  if (const_count > 0) {
    chunk->consts = al_alloc(r->al, sizeof(Value) * (size_t)const_count);
    chunk->const_count = chunk->const_cap = const_count;
    for (int i = 0; i < const_count; i++) {
      // unit until decoded: heap_intern below may not collect (no VM is
      // running yet), but leaving a half-filled pool holding garbage is the
      // kind of root the GC would follow if that ever changed.
      chunk->consts[i] = val_unit();
    }
    for (int i = 0; i < const_count && r->ok; i++) {
      chunk->consts[i] = r_const(r);
    }
  }

  fun->chunk = chunk;
}

// the operand of OP_GET_GLOBAL/OP_STRUCT/OP_ENUM/OP_MAKE_DYN is two bytes, so
// an image claiming more definitions than that could address is corrupt, not
// merely large — codegen would have refused to produce it.
#define BC_MAX_SLOTS SLOT_MAX

bool bc_load(const char *path, Allocator *al, Executable *exe, Heap *heap,
             bool gc_stress, FunDef **entry_out) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return bc_error("could not open '%s'", path);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);
  if (size < 0) {
    fclose(f);
    return bc_error("could not read '%s'", path);
  }
  uint8_t *bytes = al_alloc(al, (size_t)size);
  size_t read = fread(bytes, 1, (size_t)size, f);
  fclose(f);
  if (read != (size_t)size) {
    return bc_error("could not read '%s'", path);
  }

  BcReader r = {
      .bytes = bytes, .len = (size_t)size, .al = al, .exe = exe, .ok = true};

  const uint8_t *magic;
  if (!r_take(&r, 4, &magic) || memcmp(magic, BC_MAGIC, 4) != 0) {
    return bc_error("'%s' is not a ducktape bytecode image", path);
  }
  uint16_t version = r_u16(&r);
  if (version != BC_VERSION) {
    return bc_error("bytecode image version %u, expected %u", version,
                    BC_VERSION);
  }
  uint32_t entry_idx = r_u32(&r);

  r.string_count = (int)r_u32(&r);
  exe->global_count = (int)r_u32(&r);
  exe->struct_count = (int)r_u32(&r);
  exe->enum_count = (int)r_u32(&r);
  exe->closure_count = (int)r_u32(&r);
  exe->vtable_count = (int)r_u32(&r);
  if (!r.ok) {
    return false;
  }
  if (r.string_count < 0 || exe->global_count < 0 || exe->struct_count < 0 ||
      exe->enum_count < 0 || exe->closure_count < 0 || exe->vtable_count < 0 ||
      exe->global_count > BC_MAX_SLOTS || exe->struct_count > BC_MAX_SLOTS ||
      exe->enum_count > BC_MAX_SLOTS || exe->vtable_count > BC_MAX_SLOTS) {
    return bc_error("bytecode image: implausible section counts");
  }
  if (entry_idx >= (uint32_t)exe->global_count) {
    return bc_error("bytecode image: entry point %u is not a global",
                    entry_idx);
  }

  r.strings = al_alloc_zero(al, sizeof(StringView) * (size_t)r.string_count);
  for (int i = 0; i < r.string_count && r.ok; i++) {
    uint32_t len = r_u32(&r);
    const uint8_t *chars;
    if (!r_take(&r, len, &chars)) {
      break;
    }
    // pointed straight into the image buffer, which outlives the run: it is
    // arena memory freed with the rest of the compiler.
    r.strings[i] = (StringView){.chars = (const char *)chars, .len = (int)len};
  }
  if (!r.ok) {
    return false;
  }

  // every definition is allocated before any record is decoded — both so a
  // VAL_FUN constant can name a function further down the file, and so the
  // heap can root off complete tables (chunks still NULL) before the first
  // string constant is interned. Same ordering codegen imposes on itself.
  exe->globals =
      al_alloc_zero(al, sizeof(FunDef *) * (size_t)exe->global_count);
  exe->global_cap = exe->global_count; // an image is already monomorphised
  for (int i = 0; i < exe->global_count; i++) {
    exe->globals[i] = al_alloc_zero_for(al, FunDef);
    exe->globals[i]->slot = i;
  }
  exe->closures =
      al_alloc_zero(al, sizeof(FunDef *) * (size_t)exe->closure_count);
  exe->closure_cap = exe->closure_count;
  for (int i = 0; i < exe->closure_count; i++) {
    exe->closures[i] = al_alloc_zero_for(al, FunDef);
    exe->closures[i]->slot = -1; // reachable only through a constant
    exe->closures[i]->is_closure = true;
  }
  exe->structs =
      al_alloc_zero(al, sizeof(StructDef *) * (size_t)exe->struct_count);
  for (int i = 0; i < exe->struct_count; i++) {
    exe->structs[i] = al_alloc_zero_for(al, StructDef);
    exe->structs[i]->slot = i;
  }
  exe->enums = al_alloc_zero(al, sizeof(EnumDef *) * (size_t)exe->enum_count);
  for (int i = 0; i < exe->enum_count; i++) {
    exe->enums[i] = al_alloc_zero_for(al, EnumDef);
    exe->enums[i]->slot = i;
  }

  heap_init(heap, exe, gc_stress);
  r.heap = heap;

  for (int i = 0; i < exe->struct_count && r.ok; i++) {
    StructDef *s = exe->structs[i];
    s->name = r_str(&r);
    s->fields = r_fields(&r, &s->field_count, &s->is_tuple);
  }
  for (int i = 0; i < exe->enum_count && r.ok; i++) {
    EnumDef *e = exe->enums[i];
    e->name = r_str(&r);
    e->variant_count = r_u16(&r);
    if (e->variant_count > 0) {
      e->variants =
          al_alloc_zero(al, sizeof(VariantDef) * (size_t)e->variant_count);
    }
    for (int j = 0; j < e->variant_count && r.ok; j++) {
      VariantDef *v = &e->variants[j];
      v->name = r_str(&r);
      v->fields = r_fields(&r, &v->field_count, &v->is_tuple);
      v->tag = (uint8_t)j; // positional, as exe_assign_tags assigns them
    }
  }
  // vtables sit between the data definitions and the code, matching bc_write:
  // a vtable only ever points *into* globals, which already exist, and being
  // decoded before any chunk means an OP_MAKE_DYN operand is valid as soon as
  // the code carrying it can run.
  exe->vtables =
      al_alloc_zero(al, sizeof(VTable *) * (size_t)exe->vtable_count);
  exe->vtable_cap = exe->vtable_count;
  for (int i = 0; i < exe->vtable_count && r.ok; i++) {
    VTable *vt = al_alloc_zero_for(al, VTable);
    exe->vtables[i] = vt;
    vt->method_count = r_u16(&r);
    if (vt->method_count < 0) {
      r.ok = bc_error("bytecode image: implausible vtable size");
      break;
    }
    if (vt->method_count > 0) {
      vt->methods =
          al_alloc_zero(al, sizeof(FunDef *) * (size_t)vt->method_count);
    }
    for (int j = 0; j < vt->method_count && r.ok; j++) {
      uint32_t idx = r_u32(&r);
      if (idx == VTABLE_SLOT_EMPTY) {
        vt->methods[j] = NULL; // an excluded, never-dispatched slot
        continue;
      }
      if (idx >= (uint32_t)exe->global_count) {
        r.ok = bc_error("bytecode image: vtable method index %u out of range",
                        idx);
        break;
      }
      vt->methods[j] = exe->globals[idx];
    }
  }
  // a second pass, because a link may name a table later in the section.
  for (int i = 0; i < exe->vtable_count && r.ok; i++) {
    VTable *vt = exe->vtables[i];
    int n = r_u16(&r);
    if (n <= 0) {
      continue;
    }
    vt->upcasts = al_alloc_zero(al, sizeof(VTableUpcast) * (size_t)n);
    for (int j = 0; j < n && r.ok; j++) {
      uint16_t pair = r_u16(&r);
      uint32_t idx = r_u32(&r);
      if (idx >= (uint32_t)exe->vtable_count) {
        r.ok = bc_error("bytecode image: upcast target %u out of range", idx);
        break;
      }
      vt->upcasts[j] = (VTableUpcast){.pair = pair, .to = exe->vtables[idx]};
      vt->upcast_count = j + 1;
    }
  }

  for (int i = 0; i < exe->global_count && r.ok; i++) {
    r_fun(&r, exe->globals[i]);
  }
  for (int i = 0; i < exe->closure_count && r.ok; i++) {
    r_fun(&r, exe->closures[i]);
  }
  if (!r.ok) {
    heap_destroy(heap);
    return false;
  }

  FunDef *entry = exe->globals[entry_idx];
  if (entry->chunk == NULL) {
    heap_destroy(heap);
    return bc_error("bytecode image: entry point " SV_FMT " has no body",
                    SV_ARG(entry->name));
  }
  *entry_out = entry;
  return true;
}

bool bc_is_image(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return false;
  }
  char magic[4] = {0};
  bool match = fread(magic, 1, sizeof magic, f) == sizeof magic &&
               memcmp(magic, BC_MAGIC, sizeof magic) == 0;
  fclose(f);
  return match;
}
