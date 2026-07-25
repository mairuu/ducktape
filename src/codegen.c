#include "codegen.h"
#include "ast.h"
#include "chunk.h"
#include "object.h"
#include "scanner.h"
#include "string_utils.h"
#include "value.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CG_MAX_LOCALS 256
#define CG_MAX_BREAKS 32
#define CG_MAX_UPVALUES 256

typedef struct {
  StringView name;  // empty for hidden locals
  bool is_captured; // a nested closure captures this slot (needs closing)
} CgLocal;

// one entry in a closure's capture list: where the value comes from in the
// *enclosing* function — either that function's local slot (`is_local`) or
// its own upvalue at `index` (a variable captured further out).
typedef struct {
  bool is_local;
  uint8_t index;
} CgUpvalue;

typedef struct CgLoop {
  int start; // ip of the loop condition (OP_LOOP target for `while`)

  int break_jumps[CG_MAX_BREAKS];
  int break_count;
  int continue_jumps[CG_MAX_BREAKS];
  int continue_count;
  bool continue_is_backward; // while: continue loops back to `start`

  int continue_base; // locals kept on continue (incl. hidden iter locals)
  int break_base;    // locals kept on break (excl. hidden iter locals)

  struct CgLoop *parent;
} CgLoop;

typedef struct Cg {
  Module *m;        // the module being compiled (name lookups are module-local)
  ImplIndex *impls; // the impl set this body may select from — see Instance
  Executable *exe;  // the linked program (slot spaces, closure registry)
  Heap *heap;
  Mono *mono; // the monomorphisation queue; generic callees are enqueued here
  Chunk *chunk;
  DiagBag *diags;
  Allocator *al;

  // the instantiation this body is being compiled under: every type parameter
  // in scope bound to a concrete type. Empty for a non-generic definition, in
  // which case subst_apply is a no-op and nothing below pays for it.
  Subst subst;
  int inst_depth; // instantiations traversed to reach this body

  CgLocal locals[CG_MAX_LOCALS];
  int local_count;

  CgUpvalue upvalues[CG_MAX_UPVALUES];
  int upvalue_count;

  struct Cg *parent; // enclosing function while compiling a closure; else NULL

  CgLoop *loop;
  bool ok;

  int self_slot; // -1 outside a method; the frame slot holding `self`
} Cg;

// the one wording for every VM limitation. `what` names the construct.
static void cg_unsupported(DiagBag *diags, Span span, const char *what) {
  diag_error(diags, span, "%s is not supported by the VM yet", what);
}

static void cg_error(Cg *cg, Span span, const char *what) {
  cg_unsupported(cg->diags, span, what);
  cg->ok = false;
}

// ── monomorphisation, and the reachability walk it already was ───────────────
//
// The runtime is uniform in type arguments: instance layouts are field slots
// in declaration order, variant tags are per enum, and no opcode inspects a
// static type — which is why `?` can propagate an `Err` without rebuilding it.
// So a type argument changes exactly one thing about the code compiled from a
// body: *which function a call resolves to*. Monomorphisation therefore
// duplicates code and nothing else — one chunk per distinct tuple of type
// arguments, keyed on the definition plus that tuple.
//
// The type arguments come from the checker, which recorded on every call node
// the substitution it solved (`ExprPath.inst`, `ExprMethodCall.inst`). Those
// are written in the *enclosing* definition's terms, so a call inside a
// generic body is instantiated by pushing the caller's own bindings through
// them — `cg->subst` — which is what makes the queue reach a fixpoint instead
// of stopping one level down.
//
// Read the other way round, that queue *is* a reachability walk: a generic
// definition nothing asks for is never compiled and costs no slot. A
// non-generic definition is the same thing with nothing to specialise, so it
// arrives the same way — `mono_reach` is `mono_request` with an empty key and
// no copy, and the walk starts at `main` (`mono_seed`). Nothing is compiled
// because it was *written*; everything is compiled because something *named*
// it. Two consequences worth stating:
//   - slot pressure is a property of what a program uses, not of what its
//     imports declare. `use std::cmp::max;` no longer spends slots on the
//     `Int::cmp`/`Float::cmp` that `max` happens not to reach.
//   - a body nothing reaches is never handed to codegen, so a construct the VM
//     refuses inside one goes unreported. That was already true of every
//     generic; it is now true uniformly, which is the honest price.

static bool exe_add_global(Executable *exe, Allocator *al, FunDef *fun);
static bool exe_too_many(const char *what, int count);
static int exe_next_cap(int cap);

void mono_init(Mono *mono, Executable *exe, Heap *heap, Allocator *al) {
  *mono = (Mono){.exe = exe, .heap = heap, .al = al};
}

// does `t` still mention a type parameter or an unsolved inference variable?
// Such a type cannot key an instantiation: there would be no single body to
// compile. It means either the checker already reported an uninferable type,
// or a bound stayed abstract, so codegen refuses rather than picking a copy.
static bool type_is_concrete(const Type *t) {
  switch (t->kind) {
  case TY_GENERIC:
  case TY_UNKNOWN:
  case TY_ASSOC:
  case TY_TRAIT:
    return false;
  case TY_FUNCTION:
    for (int i = 0; i < t->as.fun.param_count; i++) {
      if (!type_is_concrete(t->as.fun.param_types[i])) {
        return false;
      }
    }
    return type_is_concrete(t->as.fun.return_type);
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (!type_is_concrete(t->as.tuple.elem_types[i])) {
        return false;
      }
    }
    return true;
  case TY_STRUCT:
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (!type_is_concrete(t->as.struc.type_args[i])) {
        return false;
      }
    }
    return true;
  case TY_ENUM:
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      if (!type_is_concrete(t->as.enm.type_args[i])) {
        return false;
      }
    }
    return true;
  case TY_ARRAY:
    return type_is_concrete(t->as.array.elem_type);
  case TY_DYN:
    // concrete, unlike the TY_TRAIT above it: `dyn Show` names one runtime
    // representation (a value plus a vtable), so it can key an instantiation
    // exactly like a struct can. That difference *is* the feature. Its
    // associated-type bindings are ordinary types and can still be abstract
    // (`dyn Iterator<Item = T>` inside a generic), so they are asked too —
    // the same question `Opt<T>` above answers about its type arguments — and
    // since milestone 28 the trait's own arguments are asked the same way
    // (`dyn Into<T>`).
    for (int i = 0; i < t->as.dyn.trait->as.trait.type_arg_count; i++) {
      if (!type_is_concrete(t->as.dyn.trait->as.trait.type_args[i])) {
        return false;
      }
    }
    for (int i = 0; i < t->as.dyn.assoc_type_count; i++) {
      if (!type_is_concrete(t->as.dyn.assoc_types[i])) {
        return false;
      }
    }
    return true;
  default:
    return true;
  }
}

// two instantiations are the same iff they share an origin and every type
// argument is the same type — pointer equality, since types are interned.
static bool inst_matches(const Instance *it, const FunDef *origin,
                         const Subst *s) {
  if (it->origin != origin || it->subst.count != s->count) {
    return false;
  }
  for (int i = 0; i < s->count; i++) {
    if (it->subst.args[i] != s->args[i]) {
      return false;
    }
  }
  return true;
}

// append a body to the worklist. The caller has already given `instance` its
// slot, which is what makes a self- or mutually-recursive request terminate:
// the memo is in place before the body that would ask again is compiled.
static void mono_enqueue(Mono *mono, FunDef *origin, FunDef *instance, Subst s,
                         int depth, ImplIndex *impls) {
  if (mono->count == mono->cap) {
    int new_cap = mono->cap == 0 ? 8 : mono->cap * 2;
    mono->insts =
        al_realloc(mono->al, mono->insts, sizeof(Instance) * (size_t)mono->cap,
                   sizeof(Instance) * (size_t)new_cap);
    mono->cap = new_cap;
  }
  mono->insts[mono->count++] = (Instance){.origin = origin,
                                          .instance = instance,
                                          .subst = s,
                                          .depth = depth,
                                          .impls = impls};
}

// find or create the copy of `origin` compiled under `s`, queueing it for
// compilation on first request. `depth` is the requesting body's depth plus
// one — only paid on a miss, since a repeat request (a recursive generic
// calling itself at the same types) returns the copy already queued and so
// never deepens. NULL if the program has run out of slots.
static FunDef *mono_request(Mono *mono, FunDef *origin, Subst s, int depth,
                            ImplIndex *impls) {
  for (int i = 0; i < mono->count; i++) {
    if (inst_matches(&mono->insts[i], origin, &s)) {
      return mono->insts[i].instance;
    }
  }

  FunDef *instance = al_alloc_zero_for(mono->al, FunDef);
  *instance = *origin; // same body, params and name; its own slot and chunk
  instance->chunk = NULL;
  if (!exe_add_global(mono->exe, mono->al, instance)) {
    return NULL;
  }
  mono_enqueue(mono, origin, instance, s, depth, impls);
  return instance;
}

// the non-generic half of the same operation: `fun` has nothing to specialise,
// so it *is* its own instance — no copy, an empty key, and `slot` itself is
// the memo. First reach hands out the slot and queues the body; every later
// one just reads the slot back.
//
// The impl set is the *defining* module's, not the requester's, and that is
// the one place this differs from `mono_request`: a bound's witness travels
// with a type argument because the argument is chosen at the call site, but a
// non-generic body selects impls entirely from where it was written — the same
// set the checker used on it. So who reaches it first cannot change what it
// compiles to.
//
// A native is reached like anything else (it needs a slot to be callable and
// first-class) but has no body to queue — one C function serves every
// instantiation, which is why this path takes generic natives too.
static FunDef *mono_reach(Mono *mono, FunDef *fun) {
  if (fun->slot != SLOT_NONE) {
    return fun;
  }
  if (!exe_add_global(mono->exe, mono->al, fun)) {
    return NULL;
  }
  if (!fun_is_native(fun)) {
    mono_enqueue(mono, fun, fun, subst_empty(), 0, &fun->module->visible_impls);
  }
  return fun;
}

bool mono_seed(Mono *mono, FunDef *entry) {
  return mono_reach(mono, entry) != NULL;
}

// what the call site instantiated one type parameter with, made concrete:
// read from the recorded arguments (`first`, then `second`), then pushed
// through the instantiation the *caller* is being compiled under. NULL when it
// cannot be pinned down, which needs a diagnostic from above.
//
// Which of the two goes first is the caller's decision and matters, because a
// `Subst` is keyed by name and a name can mean two things at once — see
// cg_inst_key.
static Type *cg_bind_param(Cg *cg, StringView name, const Subst *first,
                           const Subst *second) {
  Type *arg = first != NULL ? subst_find(first, name) : NULL;
  if (arg == NULL && second != NULL) {
    arg = subst_find(second, name);
  }
  if (arg == NULL) {
    return NULL;
  }
  arg = subst_apply(&cg->subst, arg, cg->al);
  return type_is_concrete(arg) ? arg : NULL;
}

// The key an instantiation of `fun` is memoised and compiled under: every type
// parameter in scope in its body — its impl's, then its own — bound to a
// concrete type.
static bool cg_inst_key(Cg *cg, FunDef *fun, const Subst *primary,
                        const Subst *fallback, Span span, Subst *out) {
  int impl_count = fun->impl != NULL ? fun->impl->type_param_count : 0;
  int n = impl_count + fun->type_param_count;
  StringView *names = al_alloc(cg->al, sizeof(StringView) * (size_t)n);
  Type **args = al_alloc(cg->al, sizeof(Type *) * (size_t)n);
  int k = 0;

  for (int i = 0; i < impl_count; i++) {
    StringView name = fun->impl->type_params[i]->as.generic.name;
    bool shadowed = false;
    for (int j = 0; j < fun->type_param_count; j++) {
      shadowed |= sv_equal(fun->type_params[j]->as.generic.name, name);
    }
    if (shadowed) {
      continue; // the method's own parameter of that name is bound below
    }
    names[k] = name;
    // an impl parameter is read from the *fallback* first, because that is the
    // impl's own match and nothing else can speak for it. A `Subst` is keyed by
    // name, and a call through a bound records the trait's arguments in
    // `primary` — so `impl<T: Tag> Boxed<Int> for T` against `trait Boxed<T>`
    // has two live meanings for "T", and reading primary first silently picks
    // the trait's. (An impl reached by a concrete receiver has an empty
    // fallback and its parameters in primary, so the order costs it nothing.)
    args[k++] = cg_bind_param(cg, name, fallback, primary);
  }
  for (int i = 0; i < fun->type_param_count; i++) {
    StringView name = fun->type_params[i]->as.generic.name;
    names[k] = name;
    args[k++] = cg_bind_param(cg, name, primary, fallback);
  }

  for (int i = 0; i < k; i++) {
    if (args[i] == NULL) {
      // Unreachable from a program that type-checked: an unsolved type
      // argument is already a "cannot infer type" error, and compilation stops
      // before codegen. Reported rather than asserted so a checker gap cannot
      // silently pick the wrong copy.
      diag_error(cg->diags, span,
                 "cannot instantiate '" SV_FMT "': type argument '" SV_FMT
                 "' is not known here",
                 SV_ARG(fun->name), SV_ARG(names[i]));
      cg->ok = false;
      return false;
    }
  }

  subst_init(out, names, args, k);
  return true;
}

// the definition to emit a global slot for: `fun` itself when it is not
// generic, otherwise the copy instantiated for this call site. NULL (with a
// diagnostic already reported) when there is none to emit.
static FunDef *cg_call_target(Cg *cg, FunDef *fun, const Subst *primary,
                              const Subst *fallback, Span span) {
  if (fun->native_kind == ATTR_INTRINSIC) {
    // an intrinsic has no body at all — it *is* an opcode, which a global slot
    // cannot address. A direct call is lowered inline by compile_call;
    // anything else reaches here.
    diag_error(cg->diags, span,
               "'" SV_FMT "' is an intrinsic and can only be called "
               "directly, not used as a value",
               SV_ARG(fun->name));
    cg->ok = false;
    return NULL;
  }
  // A native needs no monomorphisation even when its signature is generic: the
  // runtime is uniform in type arguments, so one C body serves every `T`.
  // `print<T>` is the whole of that case.
  if (fun_is_native(fun) || !fun_is_generic(fun)) {
    FunDef *target = mono_reach(cg->mono, fun);
    if (target == NULL) {
      cg->ok = false; // slot space exhausted; exe_add_global reported it
    }
    return target;
  }

  Subst key;
  if (!cg_inst_key(cg, fun, primary, fallback, span, &key)) {
    return NULL;
  }
  if (cg->inst_depth >= MONO_MAX_DEPTH) {
    diag_error(cg->diags, span,
               "generic instantiation is more than %d levels deep — '" SV_FMT
               "' instantiates itself at an ever-growing type",
               MONO_MAX_DEPTH, SV_ARG(fun->name));
    cg->ok = false;
    return NULL;
  }
  FunDef *target =
      mono_request(cg->mono, fun, key, cg->inst_depth + 1, cg->impls);
  if (target == NULL) {
    cg->ok = false; // slot space exhausted; exe_add_global reported it
  }
  return target;
}

static void emit(Cg *cg, uint8_t byte) { chunk_write(cg->chunk, byte); }

static void emit2(Cg *cg, uint8_t a, uint8_t b) {
  emit(cg, a);
  emit(cg, b);
}

// an op whose operand indexes a program-grown table: two bytes, big-endian.
static void emit_slot(Cg *cg, uint8_t op, int slot) {
  assert(slot >= 0 && slot < SLOT_MAX && "slot outside the operand space");
  emit(cg, op);
  chunk_write_u16(cg->chunk, (uint16_t)slot);
}

// the pool is per function but has no frame bounding it, so it is a u16 space
// like the program-wide ones — and outgrowing it is a diagnostic rather than
// the assert it used to be.
static int cg_add_const(Cg *cg, Value v) {
  int idx = chunk_add_const(cg->chunk, v);
  if (idx < 0) {
    fprintf(stderr,
            "error: a function has more than %d constants; the VM addresses "
            "at most that many (a two-byte operand)\n",
            SLOT_MAX);
    cg->ok = false;
    return 0;
  }
  return idx;
}

static void emit_const(Cg *cg, Value v) {
  emit_slot(cg, OP_CONST, cg_add_const(cg, v));
}

// decode the scanner's accepted escapes (\n \t \r \" \\ \{) and intern; the
// decoded form is never longer than the raw source slice.
static ObjString *cg_decode_string(Cg *cg, StringView raw) {
  char *buf = al_alloc(cg->al, (size_t)raw.len);
  int n = 0;
  for (int i = 0; i < raw.len; i++) {
    char c = raw.chars[i];
    if (c == '\\' && i + 1 < raw.len) {
      i++;
      char e = raw.chars[i];
      buf[n++] = e == 'n'   ? '\n'
                 : e == 't' ? '\t'
                 : e == 'r' ? '\r'
                            : e; // '"', '\\', '{': literal
    } else {
      buf[n++] = c;
    }
  }
  return heap_intern(cg->heap, buf, n);
}

// emit a forward jump with a placeholder offset; returns the operand's ip.
static int emit_jump(Cg *cg, uint8_t op) {
  emit(cg, op);
  emit2(cg, 0xff, 0xff);
  return cg->chunk->count - 2;
}

static void patch_jump(Cg *cg, int operand_ip) {
  int dist = cg->chunk->count - operand_ip - 2;
  assert(dist >= 0 && dist <= 0xffff && "jump too far");
  cg->chunk->code[operand_ip] = (uint8_t)((dist >> 8) & 0xff);
  cg->chunk->code[operand_ip + 1] = (uint8_t)(dist & 0xff);
}

static void emit_loop(Cg *cg, int target_ip) {
  emit(cg, OP_LOOP);
  int dist = cg->chunk->count - target_ip + 2;
  assert(dist >= 0 && dist <= 0xffff && "loop body too large");
  emit2(cg, (uint8_t)((dist >> 8) & 0xff), (uint8_t)(dist & 0xff));
}

// pop `n` locals' stack slots without disturbing the value on top.
static void emit_slide(Cg *cg, int n) {
  if (n > 0) {
    emit2(cg, OP_SLIDE, (uint8_t)n);
  }
}

// ── locals ───────────────────────────────────────────────────────────────────

static int cg_add_local(Cg *cg, StringView name, Span span) {
  if (cg->local_count >= CG_MAX_LOCALS) {
    diag_error(cg->diags, span, "too many locals in one function");
    cg->ok = false;
    return 0;
  }
  cg->locals[cg->local_count] = (CgLocal){.name = name};
  return cg->local_count++;
}

static int cg_find_local(Cg *cg, StringView name) {
  for (int i = cg->local_count - 1; i >= 0; i--) {
    if (cg->locals[i].name.len > 0 && sv_equal(cg->locals[i].name, name)) {
      return i;
    }
  }
  return -1;
}

// record that this closure captures `{is_local, index}`, returning the
// upvalue's index. de-duplicates so capturing the same variable twice yields
// one shared upvalue cell at runtime.
static int cg_add_upvalue(Cg *cg, bool is_local, uint8_t index, Span span) {
  for (int i = 0; i < cg->upvalue_count; i++) {
    if (cg->upvalues[i].is_local == is_local &&
        cg->upvalues[i].index == index) {
      return i;
    }
  }
  if (cg->upvalue_count >= CG_MAX_UPVALUES) {
    diag_error(cg->diags, span, "too many captured variables in one closure");
    cg->ok = false;
    return 0;
  }
  cg->upvalues[cg->upvalue_count] =
      (CgUpvalue){.is_local = is_local, .index = index};
  return cg->upvalue_count++;
}

// resolve `name` as an upvalue of `cg`: a variable owned by some enclosing
// function. returns the upvalue index, or -1 if not found in any parent.
// walking out marks the owning local `is_captured` so its defining scope
// closes the upvalue when the slot dies.
static int cg_resolve_upvalue(Cg *cg, StringView name, Span span) {
  if (cg->parent == NULL) {
    return -1;
  }
  int local = cg_find_local(cg->parent, name);
  if (local >= 0) {
    cg->parent->locals[local].is_captured = true;
    return cg_add_upvalue(cg, true, (uint8_t)local, span);
  }
  int upvalue = cg_resolve_upvalue(cg->parent, name, span);
  if (upvalue >= 0) {
    return cg_add_upvalue(cg, false, (uint8_t)upvalue, span);
  }
  return -1;
}

// if any local in [from_slot, local_count) was captured, emit a close so the
// open upvalues over those dying slots copy their values onto the heap.
static void cg_close_scope(Cg *cg, int from_slot) {
  for (int i = from_slot; i < cg->local_count; i++) {
    if (cg->locals[i].is_captured) {
      emit2(cg, OP_CLOSE_UPVALUE, (uint8_t)from_slot);
      return;
    }
  }
}

// register a nested closure's FunDef so heap_collect keeps its chunk constants
// rooted (closures are in no slot space, so exe->globals never names them).
static void cg_register_closure(Cg *cg, FunDef *fun) {
  Executable *exe = cg->exe;
  if (exe->closure_count >= exe->closure_cap) {
    int new_cap = exe->closure_cap == 0 ? 4 : exe->closure_cap * 2;
    exe->closures = al_realloc(cg->al, exe->closures,
                               sizeof(FunDef *) * (size_t)exe->closure_cap,
                               sizeof(FunDef *) * (size_t)new_cap);
    exe->closure_cap = new_cap;
  }
  exe->closures[exe->closure_count++] = fun;
}

// ── struct/enum field lookups ────────────────────────────────────────────────
//
// the checker validates field names/arity/types but doesn't reorder a
// struct/variant initializer's fields to declaration order, and pattern
// fields cache the target StructDef/VariantDef but not a per-field slot
// index — codegen derives both directly from the (small, fixed) FieldDef
// arrays already on the def.

// position of `ident` within `fields` (declaration order == runtime slot).
static int find_field_index(FieldDef *fields, int count, bool is_tuple,
                            FieldIdent ident) {
  if (is_tuple) {
    return ident.index;
  }
  for (int i = 0; i < count; i++) {
    if (sv_equal(fields[i].ident.name, ident.name)) {
      return i;
    }
  }
  assert(false && "unknown field name got past the checker");
  return -1;
}

// the initializer entry supplying `target`, regardless of source order.
static FieldInit *find_field_init(FieldInit *fields, int count, bool is_tuple,
                                  FieldIdent target) {
  for (int i = 0; i < count; i++) {
    bool match = is_tuple ? fields[i].ident.index == target.index
                          : sv_equal(fields[i].ident.name, target.name);
    if (match) {
      return &fields[i];
    }
  }
  assert(false && "field arity/name mismatch got past the checker");
  return NULL;
}

// ── expression / statement compilation ───────────────────────────────────────

static void compile_expr(Cg *cg, Expr *expr);
static void compile_closure(Cg *cg, Expr *expr);
static void compile_destructure(Cg *cg, Pattern *pat, int subject_slot);

static void compile_stmt(Cg *cg, Stmt *stmt) {
  switch (stmt->kind) {
  case STMT_EXPR:
    compile_expr(cg, stmt->as.expr_stmt.expr);
    emit(cg, OP_POP);
    break;

  case STMT_VAR: {
    StmtVar *var = &stmt->as.var_stmt;
    compile_expr(cg, var->initializer);

    if (var->binding->kind == PAT_BIND) {
      // the initializer's stack slot becomes the local
      cg_add_local(cg, var->binding->as.bind.name, stmt->span);
      break;
    }

    // destructuring: the initializer stays put as an anonymous local that the
    // pattern's accessors read from, and the names it binds are appended
    // above it. The temp costs a slot for the rest of the scope, which is
    // what keeps every bound name a plain local like any other.
    int subject_slot = cg_add_local(cg, (StringView){0}, stmt->span);
    compile_destructure(cg, var->binding, subject_slot);
    break;
  }

  case STMT_RETURN: {
    StmtReturn *ret = &stmt->as.return_stmt;
    if (ret->value != NULL) {
      compile_expr(cg, ret->value);
    } else {
      emit(cg, OP_UNIT);
    }
    emit(cg, OP_RETURN);
    break;
  }

  case STMT_BREAK: {
    CgLoop *loop = cg->loop;
    assert(loop && "break outside loop got past the checker");
    if (loop->break_count >= CG_MAX_BREAKS) {
      diag_error(cg->diags, stmt->span, "too many breaks in one loop");
      cg->ok = false;
      break;
    }
    int n = cg->local_count - loop->break_base;
    if (n > 0) {
      cg_close_scope(cg, loop->break_base); // detach captures before popping
      emit2(cg, OP_POPN, (uint8_t)n);
    }
    loop->break_jumps[loop->break_count++] = emit_jump(cg, OP_JUMP);
    break;
  }

  case STMT_CONTINUE: {
    CgLoop *loop = cg->loop;
    assert(loop && "continue outside loop got past the checker");
    int n = cg->local_count - loop->continue_base;
    if (n > 0) {
      cg_close_scope(cg, loop->continue_base); // detach captures before popping
      emit2(cg, OP_POPN, (uint8_t)n);
    }
    if (loop->continue_is_backward) {
      emit_loop(cg, loop->start);
    } else {
      if (loop->continue_count >= CG_MAX_BREAKS) {
        diag_error(cg->diags, stmt->span, "too many continues in one loop");
        cg->ok = false;
        break;
      }
      loop->continue_jumps[loop->continue_count++] = emit_jump(cg, OP_JUMP);
    }
    break;
  }

  default:
    cg_error(cg, stmt->span, "this statement");
  }
}

static void compile_block(Cg *cg, Expr *expr) {
  ExprBlock *block = &expr->as.block;
  int saved_locals = cg->local_count;

  for (int i = 0; i < block->stmt_count; i++) {
    compile_stmt(cg, block->stmts[i]);
  }

  if (block->tail_expr != NULL) {
    compile_expr(cg, block->tail_expr);
  } else {
    emit(cg, OP_UNIT);
  }

  cg_close_scope(cg, saved_locals);
  emit_slide(cg, cg->local_count - saved_locals);
  cg->local_count = saved_locals;
}

static void compile_binary(Cg *cg, Expr *expr) {
  ExprBinary *binary = &expr->as.binary;
  TokenType op = binary->op;

  // short-circuit logic
  if (op == TOKEN_AND) {
    compile_expr(cg, binary->left);
    int end = emit_jump(cg, OP_JUMP_IF_FALSE);
    emit(cg, OP_POP);
    compile_expr(cg, binary->right);
    patch_jump(cg, end);
    return;
  }
  if (op == TOKEN_OR) {
    compile_expr(cg, binary->left);
    int rhs = emit_jump(cg, OP_JUMP_IF_FALSE);
    int end = emit_jump(cg, OP_JUMP);
    patch_jump(cg, rhs);
    emit(cg, OP_POP);
    compile_expr(cg, binary->right);
    patch_jump(cg, end);
    return;
  }

  compile_expr(cg, binary->left);
  compile_expr(cg, binary->right);

  switch (op) {
  case TOKEN_PLUS:
    emit(cg, OP_ADD);
    break;
  case TOKEN_MINUS:
    emit(cg, OP_SUB);
    break;
  case TOKEN_STAR:
    emit(cg, OP_MUL);
    break;
  case TOKEN_SLASH:
    emit(cg, OP_DIV);
    break;
  case TOKEN_PERCENT:
    emit(cg, OP_MOD);
    break;
  case TOKEN_EQEQ:
    emit(cg, OP_EQ);
    break;
  case TOKEN_BANGEQ:
    emit(cg, OP_NEQ);
    break;
  case TOKEN_LT:
    emit(cg, OP_LT);
    break;
  case TOKEN_LTEQ:
    emit(cg, OP_LTEQ);
    break;
  case TOKEN_GT:
    emit(cg, OP_GT);
    break;
  case TOKEN_GTEQ:
    emit(cg, OP_GTEQ);
    break;
  default:
    cg_error(cg, expr->span, "this operator");
  }
}

// assignment to `arr[i]` (`=` or a compound `+=`/etc). stack discipline:
// [array, index, value] going into OP_INDEX_SET, which pops all three and
// pushes `value` back as the expression's result.
static void compile_index_assign(Cg *cg, Expr *expr) {
  ExprAssign *assign = &expr->as.assign;
  ExprIndex *index = &assign->target->as.index;

  compile_expr(cg, index->object); // [array]
  compile_expr(cg, index->index);  // [array, index]

  if (assign->op == TOKEN_EQ) {
    compile_expr(cg, assign->value); // [array, index, value]
  } else {
    emit2(cg, OP_DUP, 1);            // [array, index, array]
    emit2(cg, OP_DUP, 1);            // [array, index, array, index]
    emit(cg, OP_INDEX_GET);          // [array, index, current]
    compile_expr(cg, assign->value); // [array, index, current, value]
    switch (assign->op) {
    case TOKEN_PLUSEQ:
      emit(cg, OP_ADD);
      break;
    case TOKEN_MINUSEQ:
      emit(cg, OP_SUB);
      break;
    case TOKEN_STAREQ:
      emit(cg, OP_MUL);
      break;
    case TOKEN_SLASHEQ:
      emit(cg, OP_DIV);
      break;
    default:
      cg_error(cg, expr->span, "this compound assignment");
      break;
    }
  }

  emit(cg, OP_INDEX_SET);
}

// assignment to `obj.field` (`=` or a compound `+=`/etc). stack discipline:
// [obj, value] going into OP_FIELD_SET, which pops both and pushes `value` back
// as the expression's result. The receiver is compiled once — a compound op
// re-reads the field off a duplicated instance rather than re-evaluating it.
static void compile_field_assign(Cg *cg, Expr *expr) {
  ExprAssign *assign = &expr->as.assign;
  ExprField *field = &assign->target->as.field;
  uint8_t idx = (uint8_t)field->resolved_index;

  compile_expr(cg, field->object); // [obj]

  if (assign->op == TOKEN_EQ) {
    compile_expr(cg, assign->value); // [obj, value]
  } else {
    emit2(cg, OP_DUP, 0);            // [obj, obj]
    emit2(cg, OP_FIELD_GET, idx);    // [obj, current]
    compile_expr(cg, assign->value); // [obj, current, value]
    switch (assign->op) {
    case TOKEN_PLUSEQ:
      emit(cg, OP_ADD);
      break;
    case TOKEN_MINUSEQ:
      emit(cg, OP_SUB);
      break;
    case TOKEN_STAREQ:
      emit(cg, OP_MUL);
      break;
    case TOKEN_SLASHEQ:
      emit(cg, OP_DIV);
      break;
    default:
      cg_error(cg, expr->span, "this compound assignment");
      break;
    }
  }

  emit2(cg, OP_FIELD_SET, idx);
}

static void compile_assign(Cg *cg, Expr *expr) {
  ExprAssign *assign = &expr->as.assign;

  if (assign->target->kind == EXPR_INDEX) {
    compile_index_assign(cg, expr);
    return;
  }

  if (assign->target->kind == EXPR_FIELD) {
    compile_field_assign(cg, expr);
    return;
  }

  if (assign->target->kind != EXPR_PATH ||
      assign->target->as.path_expr.path.count != 1) {
    cg_error(cg, assign->target->span, "assignment to this target");
    emit(cg, OP_UNIT);
    return;
  }

  StringView name = assign->target->as.path_expr.path.segments[0].name;
  // a target is either a local of this function or an upvalue captured from an
  // enclosing one; both read/write through the same get/set opcode pair.
  int slot = cg_find_local(cg, name);
  uint8_t get_op, set_op, operand;
  if (slot >= 0) {
    get_op = OP_GET_LOCAL;
    set_op = OP_SET_LOCAL;
    operand = (uint8_t)slot;
  } else {
    int upvalue = cg_resolve_upvalue(cg, name, assign->target->span);
    if (upvalue < 0) {
      cg_error(cg, assign->target->span, "assignment to a non-local");
      emit(cg, OP_UNIT);
      return;
    }
    get_op = OP_GET_UPVALUE;
    set_op = OP_SET_UPVALUE;
    operand = (uint8_t)upvalue;
  }

  if (assign->op == TOKEN_EQ) {
    compile_expr(cg, assign->value);
  } else {
    emit2(cg, get_op, operand);
    compile_expr(cg, assign->value);
    switch (assign->op) {
    case TOKEN_PLUSEQ:
      emit(cg, OP_ADD);
      break;
    case TOKEN_MINUSEQ:
      emit(cg, OP_SUB);
      break;
    case TOKEN_STAREQ:
      emit(cg, OP_MUL);
      break;
    case TOKEN_SLASHEQ:
      emit(cg, OP_DIV);
      break;
    default:
      cg_error(cg, expr->span, "this compound assignment");
      break;
    }
  }

  emit2(cg, set_op, operand); // value stays as the expr result
}

static void compile_if(Cg *cg, Expr *expr) {
  ExprIf *if_ = &expr->as.if_expr;

  compile_expr(cg, if_->condition);
  int else_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);
  compile_expr(cg, if_->then_block);
  int end_jump = emit_jump(cg, OP_JUMP);

  patch_jump(cg, else_jump);
  emit(cg, OP_POP);
  if (if_->else_branch != NULL) {
    compile_expr(cg, if_->else_branch);
  } else {
    emit(cg, OP_UNIT);
  }
  patch_jump(cg, end_jump);
}

static void compile_while(Cg *cg, Expr *expr) {
  ExprWhile *wh = &expr->as.while_expr;

  CgLoop loop = {
      .start = cg->chunk->count,
      .continue_is_backward = true,
      .continue_base = cg->local_count,
      .break_base = cg->local_count,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  compile_expr(cg, wh->condition);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);

  compile_expr(cg, wh->body);
  emit(cg, OP_POP);
  emit_loop(cg, loop.start);

  patch_jump(cg, exit_jump);
  emit(cg, OP_POP); // the condition
  for (int i = 0; i < loop.break_count; i++) {
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

static void compile_for_range(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  int saved_locals = cg->local_count;

  // [range, i] as hidden + loop locals
  compile_expr(cg, for_->iterable);
  int range_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit2(cg, OP_GET_LOCAL, (uint8_t)range_slot);
  emit(cg, OP_RANGE_START);
  int i_slot = cg_add_local(cg, for_->var_name, for_->var_span);

  CgLoop loop = {
      .continue_is_backward = false, // continue jumps forward to the increment
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;
  emit2(cg, OP_GET_LOCAL, (uint8_t)range_slot);
  emit2(cg, OP_GET_LOCAL, (uint8_t)i_slot);
  emit(cg, OP_RANGE_TEST);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);

  compile_expr(cg, for_->body);
  emit(cg, OP_POP);

  // increment; forward `continue`s land here
  for (int i = 0; i < loop.continue_count; i++) {
    patch_jump(cg, loop.continue_jumps[i]);
  }
  emit2(cg, OP_GET_LOCAL, (uint8_t)i_slot);
  emit_const(cg, val_int(1));
  emit(cg, OP_ADD);
  emit2(cg, OP_SET_LOCAL, (uint8_t)i_slot);
  emit(cg, OP_POP);
  emit_loop(cg, loop_start);

  patch_jump(cg, exit_jump);
  emit(cg, OP_POP);                            // the test result
  cg_close_scope(cg, saved_locals);            // detach a captured loop var
  emit2(cg, OP_POPN, 2);                       // i, range
  for (int i = 0; i < loop.break_count; i++) { // breaks pop their own locals
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  cg->local_count = saved_locals;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

static void compile_for_array(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  int saved_locals = cg->local_count;

  // [arr, idx, var] as hidden + loop locals; var is a placeholder (unit)
  // until the first bounds-checked read.
  compile_expr(cg, for_->iterable);
  int arr_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit_const(cg, val_int(0));
  int idx_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit(cg, OP_UNIT);
  int var_slot = cg_add_local(cg, for_->var_name, for_->var_span);

  CgLoop loop = {
      .continue_is_backward = false, // continue jumps forward to the increment
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;
  emit2(cg, OP_GET_LOCAL, (uint8_t)idx_slot);
  emit2(cg, OP_GET_LOCAL, (uint8_t)arr_slot);
  emit(cg, OP_LEN);
  emit(cg, OP_LT);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);

  emit2(cg, OP_GET_LOCAL, (uint8_t)arr_slot);
  emit2(cg, OP_GET_LOCAL, (uint8_t)idx_slot);
  emit(cg, OP_INDEX_GET);
  emit2(cg, OP_SET_LOCAL, (uint8_t)var_slot);
  emit(cg, OP_POP);

  compile_expr(cg, for_->body);
  emit(cg, OP_POP);

  // increment; forward `continue`s land here
  for (int i = 0; i < loop.continue_count; i++) {
    patch_jump(cg, loop.continue_jumps[i]);
  }
  emit2(cg, OP_GET_LOCAL, (uint8_t)idx_slot);
  emit_const(cg, val_int(1));
  emit(cg, OP_ADD);
  emit2(cg, OP_SET_LOCAL, (uint8_t)idx_slot);
  emit(cg, OP_POP);
  emit_loop(cg, loop_start);

  patch_jump(cg, exit_jump);
  emit(cg, OP_POP);                            // the test result
  cg_close_scope(cg, saved_locals);            // detach a captured loop var
  emit2(cg, OP_POPN, 3);                       // var, idx, arr
  for (int i = 0; i < loop.break_count; i++) { // breaks pop their own locals
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  cg->local_count = saved_locals;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

static bool cg_emit_target(Cg *cg, FunDef *fun, const Subst *primary,
                           const Subst *fallback, Span span);

// `for x in it` over a user iterator (neither array nor range): drive
// `it.next()` — an `Option<Item>` each turn — binding `Some(x)` and ending on
// `None`. The scaffold is compile_for_array's — the iterator lives in a hidden
// local so its cursor advances *in place* across calls, where re-evaluating the
// `it` expression each turn would restart the sequence — and the Option unwrap
// is compile_propagate's (tag test, then field 0). The checker left the
// resolved `next` call and the two variants on the node.
static void compile_for_iter(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  ExprMethodCall *mc = &for_->next_call->as.method_call;
  FunDef *next = mc->resolved_method != NULL ? mc->resolved_method->fun
                                             : mc->resolved_default;
  if (next == NULL) {
    cg_error(cg, expr->span, "this iterator");
    emit(cg, OP_UNIT);
    return;
  }

  int saved_locals = cg->local_count;

  // [iter, var]: `iter` is the receiver, whose cursor advances in place; `var`
  // is a placeholder (unit) until each `Some` binds it, reused every turn.
  compile_expr(cg, for_->iterable);
  int iter_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit(cg, OP_UNIT);
  int var_slot = cg_add_local(cg, for_->var_name, for_->var_span);

  CgLoop loop = {
      .continue_is_backward = true, // `continue` re-drives next()
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;

  // opt = iter.next() — the receiver is the hidden local, not a re-eval
  Subst empty = subst_empty();
  if (!cg_emit_target(cg, next, &mc->inst, &empty, expr->span)) {
    cg->loop = loop.parent;
    cg->local_count = saved_locals;
    emit(cg, OP_UNIT);
    return;
  }
  emit2(cg, OP_GET_LOCAL, (uint8_t)iter_slot);
  emit2(cg, OP_CALL, (uint8_t)next->param_count); // [opt]

  // Some -> bind and run the body; None -> exit
  emit2(cg, OP_DUP, 0);                            // [opt, opt]
  emit(cg, OP_TAG);                                // [opt, tag]
  emit_const(cg, val_int(for_->some_variant->tag));
  emit(cg, OP_EQ);                                 // [opt, is_some]
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE); // peeks is_some
  emit(cg, OP_POP);                                // is_some (true) -> [opt]
  emit2(cg, OP_FIELD_GET, 0);                      // [payload] (Some.0)
  emit2(cg, OP_SET_LOCAL, (uint8_t)var_slot);      // var = payload; [payload]
  emit(cg, OP_POP);                                // []

  compile_expr(cg, for_->body);
  emit(cg, OP_POP);
  emit_loop(cg, loop_start);

  patch_jump(cg, exit_jump);                    // [opt, is_some]
  emit(cg, OP_POP);                             // is_some -> [opt]
  emit(cg, OP_POP);                             // the None instance -> []
  cg_close_scope(cg, saved_locals);            // detach a captured loop var
  emit2(cg, OP_POPN, 2);                        // var, iter
  for (int i = 0; i < loop.break_count; i++) { // breaks pop their own locals
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  cg->local_count = saved_locals;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

static void compile_for(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  Type *iter_ty = for_->iterable->resolved_type;
  assert(iter_ty && "for-loop iterable unresolved after checking");
  if (iter_ty->kind == TY_ARRAY) {
    compile_for_array(cg, expr);
  } else if (iter_ty->kind == TY_RANGE) {
    compile_for_range(cg, expr);
  } else {
    compile_for_iter(cg, expr);
  }
}

// push a call target's global slot, instantiating it if it is generic.
static bool cg_emit_target(Cg *cg, FunDef *fun, const Subst *primary,
                           const Subst *fallback, Span span) {
  FunDef *target = cg_call_target(cg, fun, primary, fallback, span);
  if (target == NULL) {
    emit(cg, OP_UNIT);
    return false;
  }
  emit_slot(cg, OP_GET_GLOBAL, target->slot);
  return true;
}

static void compile_call(Cg *cg, Expr *expr) {
  ExprCall *call = &expr->as.call;
  Expr *callee = call->callee;

  // an @intrinsic call *is* its opcode: push the arguments and emit it, with
  // no callee value and no frame. Which FunDef the callee names is the
  // checker's answer, already on the node — including through an alias
  // (`use std::array::len as size;`), which a name match here could not see.
  FunDef *direct =
      callee->kind == EXPR_PATH ? callee->as.path_expr.resolved_fun : NULL;
  if (direct != NULL && direct->native_kind == ATTR_INTRINSIC) {
    for (int i = 0; i < call->arg_count; i++) {
      compile_expr(cg, call->args[i]);
    }
    emit(cg, (OpCode)direct->intrinsic_op);
    return;
  }

  compile_expr(cg, callee);
  for (int i = 0; i < call->arg_count; i++) {
    compile_expr(cg, call->args[i]);
  }
  emit2(cg, OP_CALL, (uint8_t)call->arg_count);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Trait objects
// ═══════════════════════════════════════════════════════════════════════════════

// Bind `Self` and the trait's own type parameters, which together are the
// type parameters a trait's default body is generic over (milestone 12 gave it
// `Self`; a generic trait adds the rest). The trait reference is what names
// them, so this is the same substitution `trait_ref_subst` builds in the
// checker, plus the receiver.
static Subst cg_trait_ref_subst(Cg *cg, Type *trait_ref, Type *self) {
  TraitDef *def = trait_ref->as.trait.def;
  int n = trait_ref->as.trait.type_arg_count;
  StringView *names = al_alloc(cg->al, sizeof(StringView) * (size_t)(n + 1));
  Type **args = al_alloc(cg->al, sizeof(Type *) * (size_t)(n + 1));
  for (int i = 0; i < n; i++) {
    names[i] = def->type_params[i]->as.generic.name;
    args[i] = trait_ref->as.trait.type_args[i];
  }
  names[n] = sv_from_cstr("Self");
  args[n] = self;

  Subst s;
  subst_init(&s, names, args, n + 1);
  return s;
}

// The body one vtable slot points at, for a concrete `self`: the impl's own
// method, or the trait's default body when the impl omitted it. This is the
// same two-way choice `compile_method_call` makes for a bound receiver — a
// vtable is just that choice made ahead of time, for every method at once.
static FunDef *cg_dyn_slot_target(Cg *cg, Type *trait_ref, Type *self,
                                  StringView name, Span span) {
  ImplMatch match;
  MethodDef *method = impl_index_method(
      cg->impls, self, trait_ref, name, /*ret_hint=*/NULL, &match,
      /*infer=*/NULL, /*bare_path=*/false, span, cg->al);
  if (method != NULL) {
    return cg_call_target(cg, method->fun, &match.subst, NULL, span);
  }

  ImplDef *via_impl = NULL;
  TraitDef *via_trait = NULL;
  Subst via_subst = subst_empty();
  TraitMethodDef *inherited =
      impl_index_default_method(cg->impls, self, trait_ref, name, &via_impl,
                                &via_trait, &via_subst, cg->al);
  if (inherited == NULL || inherited->default_impl == NULL) {
    return NULL;
  }

  // the default body is a generic function over `Self` and the trait's own
  // type parameters (milestone 12, extended in 28), so binding those names
  // instantiates it — and the trait reference is exactly what names them.
  Subst s = cg_trait_ref_subst(cg, trait_ref, self);
  return cg_call_target(cg, inherited->default_impl, &s, &via_subst, span);
}

// find or build the vtable for coercing `self` to `dyn trait`, returning its
// slot. Memoised on the (trait, self) pair so every coercion of one type to
// one trait shares a table.
static int cg_vtable_for(Cg *cg, Type *trait_ref, Type *self, Span span) {
  TraitDef *trait = trait_ref->as.trait.def;
  Mono *mono = cg->mono;
  for (int i = 0; i < mono->vtable_count; i++) {
    if (mono->vtables[i].trait == trait_ref &&
        mono->vtables[i].self_type == self) {
      return mono->vtables[i].index;
    }
  }

  Executable *exe = mono->exe;
  if (exe->vtable_count == SLOT_MAX) {
    exe_too_many("trait-object vtables", exe->vtable_count + 1);
    cg->ok = false;
    return -1;
  }
  if (exe->vtable_count == exe->vtable_cap) {
    int new_cap = exe_next_cap(exe->vtable_cap);
    exe->vtables = al_realloc(cg->al, exe->vtables,
                              sizeof(VTable *) * (size_t)exe->vtable_cap,
                              sizeof(VTable *) * (size_t)new_cap);
    exe->vtable_cap = new_cap;
  }

  // Reserve the slot *before* filling the table in. Compiling a method body
  // can reach this same (trait, self) pair again — a `dyn Show` handed back
  // to something that coerces it — and without the reservation that recursion
  // would allocate a second table forever. Same shape as `mono_request`
  // memoising an instance before its body is compiled.
  int index = exe->vtable_count++;
  VTable *vt = al_alloc_zero_for(cg->al, VTable);
  vt->method_count = trait->method_count;
  vt->methods =
      al_alloc_zero(cg->al, sizeof(FunDef *) * (size_t)trait->method_count);
  exe->vtables[index] = vt;

  if (mono->vtable_count == mono->vtable_cap) {
    int new_cap = mono->vtable_cap == 0 ? 4 : mono->vtable_cap * 2;
    mono->vtables = al_realloc(mono->al, mono->vtables,
                               sizeof(DynVTable) * (size_t)mono->vtable_cap,
                               sizeof(DynVTable) * (size_t)new_cap);
    mono->vtable_cap = new_cap;
  }
  mono->vtables[mono->vtable_count++] =
      (DynVTable){.trait = trait_ref, .self_type = self, .index = index};

  for (int i = 0; i < trait->method_count; i++) {
    FunDef *target =
        cg_dyn_slot_target(cg, trait_ref, self, trait->methods[i].name, span);
    if (target == NULL) {
      char buf[64];
      type_sprintf(self, buf, sizeof(buf));
      diag_error(cg->diags, span,
                 "cannot build a trait object: '%s' has no body for '" SV_FMT
                 "::" SV_FMT "'",
                 buf, SV_ARG(trait->name), SV_ARG(trait->methods[i].name));
      cg->ok = false;
      return -1;
    }
    vt->methods[i] = target;
  }

  return index;
}

// wrap the value on top of the stack as a trait object. `expr->coerce_dyn` is
// the checker's answer that this has to happen; the concrete type comes from
// the expression, pushed through whatever instantiation is being compiled.
static void compile_coerce_dyn(Cg *cg, Expr *expr) {
  Type *self = expr->resolved_type;
  if (self == NULL) {
    cg_error(cg, expr->span, "this trait-object coercion");
    return;
  }
  self = subst_apply(&cg->subst, self, cg->al);
  if (!type_is_concrete(self)) {
    char buf[64];
    type_sprintf(self, buf, sizeof(buf));
    diag_error(cg->diags, expr->span,
               "cannot build a trait object from '%s': the concrete type is "
               "not known here",
               buf);
    cg->ok = false;
    return;
  }

  Type *trait_ref = subst_apply(&cg->subst, expr->coerce_dyn, cg->al);
  int slot = cg_vtable_for(cg, trait_ref, self, expr->span);
  if (slot < 0) {
    return;
  }
  emit_slot(cg, OP_MAKE_DYN, slot);
}

// `obj.method(args)`: the checker elides `self` from `mc->args` (it's
// `mc->object` instead), matching it against whichever param position in
// `fun->params` actually has `is_self` set (checked generically, not
// assumed to be 0 — see resolve_method_call_expr) — so codegen interleaves
// `mc->object` back in at that same position when pushing arguments.
// The body a call through a bound means, once the receiver is concrete: the
// impl's own method, or the trait's default body when the impl omitted it.
// `*out_impl_subst` is the impl's own type params as the receiver pinned them.
//
// Shared by a method call on a bounded receiver and by a path qualified by a
// type parameter (`T::from(v)`), which are two spellings of one dispatch —
// the second having no receiver to carry the abstract self type.
static FunDef *cg_bound_target(Cg *cg, Type *self, Type *trait_ref,
                               StringView name, Subst *out_impl_subst,
                               Span span) {
  *out_impl_subst = subst_empty();

  ImplMatch match;
  MethodDef *method = impl_index_method(
      cg->impls, self, trait_ref, name, /*ret_hint=*/NULL, &match,
      /*infer=*/NULL, /*bare_path=*/false, span, cg->al);
  if (method != NULL) {
    *out_impl_subst = match.subst;
    return method->fun;
  }

  // an applicable impl exists (the checker enforced the bound) but does not
  // define the name, so the body is the trait's default. It is instantiated on
  // `Self`, which the call node's `inst` carries.
  ImplDef *via_impl = NULL;
  TraitDef *via_trait = NULL;
  Subst via_subst = subst_empty();
  TraitMethodDef *inherited =
      impl_index_default_method(cg->impls, self, trait_ref, name, &via_impl,
                                &via_trait, &via_subst, cg->al);
  if (inherited == NULL) {
    return NULL;
  }
  return inherited->default_impl;
}

static void compile_method_call(Cg *cg, Expr *expr) {
  ExprMethodCall *mc = &expr->as.method_call;

  FunDef *fun = NULL;
  Subst impl_subst = subst_empty();

  if (mc->resolved_method != NULL) {
    fun = mc->resolved_method->fun;
  } else if (mc->bound_trait != NULL) {
    // dispatch through a trait bound. The checker only knew the receiver
    // abstractly (`T: Show`); pushing it through this instantiation makes it
    // concrete, and the impl index then names the body — which is the whole
    // reason generic code has to be monomorphised rather than erased.
    Type *self = subst_apply(&cg->subst, mc->bound_self, cg->al);

    // ...unless it is a trait object, where it stays abstract on purpose.
    // The receiver carries the table, so the slot is all codegen picks: the
    // position the trait fixes for this method name.
    if (self->kind == TY_DYN) {
      TraitDef *trait = dyn_trait_def(self);
      int index = -1;
      for (int i = 0; i < trait->method_count; i++) {
        if (sv_equal(trait->methods[i].name, mc->method_name)) {
          index = i;
          break;
        }
      }
      if (index < 0) {
        cg_error(cg, expr->span, "this method call");
        return;
      }
      // OP_DYN_METHOD leaves [fun, receiver], so the arguments push straight
      // on top of it and the ordinary OP_CALL below closes the call.
      compile_expr(cg, mc->object);
      emit2(cg, OP_DYN_METHOD, (uint8_t)index);
      for (int i = 0; i < mc->arg_count; i++) {
        compile_expr(cg, mc->args[i]);
      }
      emit2(cg, OP_CALL, (uint8_t)(mc->arg_count + 1));
      return;
    }

    // which trait the bound named, in this instantiation's terms: two impls
    // of one generic trait for one type differ only here.
    Type *trait_ref = subst_apply(&cg->subst, mc->bound_trait, cg->al);

    fun = cg_bound_target(cg, self, trait_ref, mc->method_name, &impl_subst,
                          expr->span);
    if (fun == NULL) {
      cg_error(cg, expr->span, "this method call");
      return;
    }
  } else if (mc->resolved_default != NULL) {
    // the receiver's impl omitted the method: the body is the trait's default,
    // compiled once per receiver type like any other generic definition.
    fun = mc->resolved_default;
  } else {
    cg_error(cg, expr->span, "this method call");
    return;
  }

  // an @intrinsic method *is* its opcode: push the receiver at its `self`
  // position, the arguments interleaved as usual, then emit the opcode inline —
  // no slot, no frame, no OP_CALL. This is compile_call's direct-intrinsic path
  // with the receiver folded in, and it is why the ATTR_INTRINSIC guard in
  // cg_call_target (an intrinsic has no slot to address) is never reached for a
  // method call.
  if (fun->native_kind == ATTR_INTRINSIC) {
    int ki = 0;
    for (int i = 0; i < fun->param_count; i++) {
      if (fun->params[i].is_self) {
        compile_expr(cg, mc->object);
      } else {
        compile_expr(cg, mc->args[ki++]);
      }
    }
    emit(cg, (OpCode)fun->intrinsic_op);
    return;
  }

  if (!cg_emit_target(cg, fun, &mc->inst, &impl_subst, expr->span)) {
    return;
  }
  int k = 0;
  for (int i = 0; i < fun->param_count; i++) {
    if (fun->params[i].is_self) {
      compile_expr(cg, mc->object);
    } else {
      compile_expr(cg, mc->args[k++]);
    }
  }
  emit2(cg, OP_CALL, (uint8_t)fun->param_count);
}

// `expr?`: Ok/Err are single-field tuple variants of the same enum as the
// enclosing function's return type (enforced by the checker), so an `Err`
// value propagates unchanged — no reconstruction, since generic type
// arguments are erased at runtime and the payload doesn't move.
static void compile_propagate(Cg *cg, Expr *expr) {
  ExprPropagate *prop = &expr->as.propagate;

  compile_expr(cg, prop->operand); // [instance]
  emit2(cg, OP_DUP, 0);            // [instance, instance]
  emit(cg, OP_TAG);                // [instance, tag]
  emit_const(cg, val_int(prop->ok_variant->tag));
  emit(cg, OP_EQ); // [instance, is_ok]

  int err_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);           // is_ok was true
  emit2(cg, OP_FIELD_GET, 0); // [payload]
  int end_jump = emit_jump(cg, OP_JUMP);

  patch_jump(cg, err_jump);
  emit(cg, OP_POP);    // is_ok was false; [instance]
  emit(cg, OP_RETURN); // propagate Err(e) to the caller unchanged

  patch_jump(cg, end_jump);
}

// A struct or an enum reaches its slot space the same way a function does:
// when something *constructs* one. Neither is reached by being declared, nor
// by being matched — a pattern tests a tag and reads fields by index, and
// neither operand names the definition. So a type a program only ever takes
// apart costs nothing in either space.
static bool cg_reach_struct(Cg *cg, StructDef *def) {
  if (def->slot != SLOT_NONE) {
    return true;
  }
  if (cg->exe->struct_count == SLOT_MAX) {
    exe_too_many("structs", cg->exe->struct_count + 1);
    cg->ok = false;
    return false;
  }
  if (cg->exe->struct_count == cg->exe->struct_cap) {
    int new_cap = exe_next_cap(cg->exe->struct_cap);
    cg->exe->structs =
        al_realloc(cg->al, cg->exe->structs,
                   sizeof(StructDef *) * (size_t)cg->exe->struct_cap,
                   sizeof(StructDef *) * (size_t)new_cap);
    cg->exe->struct_cap = new_cap;
  }
  def->slot = cg->exe->struct_count;
  cg->exe->structs[cg->exe->struct_count++] = def;
  return true;
}

static bool cg_reach_enum(Cg *cg, EnumDef *def) {
  if (def->slot != SLOT_NONE) {
    return true;
  }
  if (cg->exe->enum_count == SLOT_MAX) {
    exe_too_many("enums", cg->exe->enum_count + 1);
    cg->ok = false;
    return false;
  }
  if (cg->exe->enum_count == cg->exe->enum_cap) {
    int new_cap = exe_next_cap(cg->exe->enum_cap);
    cg->exe->enums = al_realloc(cg->al, cg->exe->enums,
                                sizeof(EnumDef *) * (size_t)cg->exe->enum_cap,
                                sizeof(EnumDef *) * (size_t)new_cap);
    cg->exe->enum_cap = new_cap;
  }
  def->slot = cg->exe->enum_count;
  cg->exe->enums[cg->exe->enum_count++] = def;
  return true;
}

// compiles field values in *declaration* order (matching the runtime
// instance layout), not the initializer's source order.
static void compile_struct_init(Cg *cg, Expr *expr) {
  ExprStructInit *init = &expr->as.struct_init;
  StructDef *def = init->resolved_struct->as.struc.def;

  if (!cg_reach_struct(cg, def)) {
    emit(cg, OP_UNIT);
    return;
  }
  for (int i = 0; i < def->field_count; i++) {
    FieldInit *fi = find_field_init(init->fields, init->field_count,
                                    def->is_tuple, def->fields[i].ident);
    compile_expr(cg, fi->value);
  }
  emit_slot(cg, OP_STRUCT, def->slot);
}

static void compile_variant_init(Cg *cg, Expr *expr) {
  ExprVariant *init = &expr->as.variant;
  EnumDef *enum_def = init->resolved_enum->as.enm.def;
  VariantDef *variant = init->resolved_variant;

  if (!cg_reach_enum(cg, enum_def)) {
    emit(cg, OP_UNIT);
    return;
  }
  for (int i = 0; i < variant->field_count; i++) {
    FieldInit *fi =
        find_field_init(init->fields, init->field_count, variant->is_tuple,
                        variant->fields[i].ident);
    compile_expr(cg, fi->value);
  }
  emit_slot(cg, OP_ENUM, enum_def->slot);
  emit(cg, variant->tag);
}

// ── pattern matching ─────────────────────────────────────────────────────────
//
// a match subject is stored once in a hidden local; a pattern's "location"
// relative to that local is a chain of field indices (an Accessor). Each arm
// compiles in two passes over its pattern: `compile_pattern_test` emits the
// runtime checks (literal equality, enum tag), short-circuiting to the next
// arm on failure via `fails`; `compile_pattern_bind` (run only once the test
// passes) declares a local for every name the pattern binds. Splitting the
// passes avoids threading partial-bind cleanup through the test logic — on
// failure nothing has been bound yet, so falling through to the next arm
// never needs to unwind anything.

#define CG_MAX_ACCESSOR_DEPTH 8
#define CG_MAX_MATCH_JUMPS 64

typedef struct {
  int base_slot;
  uint8_t path[CG_MAX_ACCESSOR_DEPTH];
  int path_len;
} Accessor;

typedef struct {
  int ips[CG_MAX_MATCH_JUMPS];
  int count;
} JumpList;

static void jump_list_push(Cg *cg, JumpList *list, Span span, int ip) {
  if (list->count >= CG_MAX_MATCH_JUMPS) {
    diag_error(cg->diags, span, "match arm has too many pattern tests");
    cg->ok = false;
    return;
  }
  list->ips[list->count++] = ip;
}

static void jump_list_patch(Cg *cg, JumpList *list) {
  for (int i = 0; i < list->count; i++) {
    patch_jump(cg, list->ips[i]);
  }
}

// pushes the value at `acc`'s location: the subject local, then one
// OP_FIELD_GET per accessor path segment.
static void emit_accessor(Cg *cg, const Accessor *acc) {
  emit2(cg, OP_GET_LOCAL, (uint8_t)acc->base_slot);
  for (int i = 0; i < acc->path_len; i++) {
    emit2(cg, OP_FIELD_GET, acc->path[i]);
  }
}

static Accessor accessor_field(Cg *cg, Accessor acc, Span span, int idx) {
  if (acc.path_len >= CG_MAX_ACCESSOR_DEPTH) {
    diag_error(cg->diags, span, "pattern is nested too deeply");
    cg->ok = false;
    return acc;
  }
  acc.path[acc.path_len++] = (uint8_t)idx;
  return acc;
}

static void compile_pattern_test(Cg *cg, Pattern *pat, Accessor acc,
                                 JumpList *fails) {
  switch (pat->kind) {
  case PAT_WILDCARD:
  case PAT_BIND:
    break; // always matches; nothing to test

  case PAT_LITERAL:
    emit_accessor(cg, &acc);
    compile_expr(cg, pat->as.literal_expr);
    emit(cg, OP_EQ);
    jump_list_push(cg, fails, pat->span, emit_jump(cg, OP_JUMP_IF_FALSE));
    emit(cg, OP_POP);
    break;

  case PAT_TUPLE:
    for (int i = 0; i < pat->as.tuple.count; i++) {
      compile_pattern_test(cg, pat->as.tuple.elems[i],
                           accessor_field(cg, acc, pat->span, i), fails);
    }
    break;

  case PAT_STRUCT: {
    StructDef *def = pat->as.struc.resolved_struct;
    for (int i = 0; i < pat->as.struc.field_count; i++) {
      FieldPat *fp = &pat->as.struc.fields[i];
      if (fp->sub_pattern == NULL) {
        continue; // shorthand bind: always matches
      }
      int idx = find_field_index(def->fields, def->field_count, def->is_tuple,
                                 fp->ident);
      compile_pattern_test(cg, fp->sub_pattern,
                           accessor_field(cg, acc, fp->span, idx), fails);
    }
    break;
  }

  case PAT_VARIANT: {
    VariantDef *variant = pat->as.variant.resolved_variant;

    emit_accessor(cg, &acc);
    emit(cg, OP_TAG);
    emit_const(cg, val_int(variant->tag));
    emit(cg, OP_EQ);
    jump_list_push(cg, fails, pat->span, emit_jump(cg, OP_JUMP_IF_FALSE));
    emit(cg, OP_POP);

    for (int i = 0; i < pat->as.variant.field_count; i++) {
      FieldPat *fp = &pat->as.variant.fields[i];
      if (fp->sub_pattern == NULL) {
        continue;
      }
      int idx = find_field_index(variant->fields, variant->field_count,
                                 variant->is_tuple, fp->ident);
      compile_pattern_test(cg, fp->sub_pattern,
                           accessor_field(cg, acc, fp->span, idx), fails);
    }
    break;
  }
  }
}

// declares a local for every name the pattern binds; only reached once
// `compile_pattern_test` has already succeeded.
static void compile_pattern_bind(Cg *cg, Pattern *pat, Accessor acc) {
  switch (pat->kind) {
  case PAT_WILDCARD:
  case PAT_LITERAL:
    break;

  case PAT_BIND:
    emit_accessor(cg, &acc);
    cg_add_local(cg, pat->as.bind.name, pat->span);
    break;

  case PAT_TUPLE:
    for (int i = 0; i < pat->as.tuple.count; i++) {
      compile_pattern_bind(cg, pat->as.tuple.elems[i],
                           accessor_field(cg, acc, pat->span, i));
    }
    break;

  case PAT_STRUCT: {
    StructDef *def = pat->as.struc.resolved_struct;
    for (int i = 0; i < pat->as.struc.field_count; i++) {
      FieldPat *fp = &pat->as.struc.fields[i];
      int idx = find_field_index(def->fields, def->field_count, def->is_tuple,
                                 fp->ident);
      Accessor field_acc = accessor_field(cg, acc, fp->span, idx);
      if (fp->sub_pattern != NULL) {
        compile_pattern_bind(cg, fp->sub_pattern, field_acc);
      } else {
        emit_accessor(cg, &field_acc);
        cg_add_local(cg, fp->ident.name, fp->span);
      }
    }
    break;
  }

  case PAT_VARIANT: {
    VariantDef *variant = pat->as.variant.resolved_variant;
    for (int i = 0; i < pat->as.variant.field_count; i++) {
      FieldPat *fp = &pat->as.variant.fields[i];
      int idx = find_field_index(variant->fields, variant->field_count,
                                 variant->is_tuple, fp->ident);
      Accessor field_acc = accessor_field(cg, acc, fp->span, idx);
      if (fp->sub_pattern != NULL) {
        compile_pattern_bind(cg, fp->sub_pattern, field_acc);
      } else {
        emit_accessor(cg, &field_acc);
        cg_add_local(cg, fp->ident.name, fp->span);
      }
    }
    break;
  }
  }
}

// a `var` binding is a one-arm match with no guard and no body: the subject is
// already in `subject_slot`, so the whole statement is the two pattern passes.
// The checker rejects a refutable binding, but its answer is tri-state — an
// unsolved column type reports nothing — so the tests are still emitted and
// still trap through OP_MATCH_FAIL rather than binding from a value that never
// had the shape. For an irrefutable pattern `compile_pattern_test` emits
// nothing at all, so this costs the common case zero instructions.
static void compile_destructure(Cg *cg, Pattern *pat, int subject_slot) {
  Accessor acc = {.base_slot = subject_slot};
  JumpList fails = {0};

  compile_pattern_test(cg, pat, acc, &fails);

  if (fails.count > 0) {
    int ok_jump = emit_jump(cg, OP_JUMP);
    jump_list_patch(cg, &fails);
    emit(cg, OP_POP); // the outstanding false from whichever test failed
    emit(cg, OP_MATCH_FAIL);
    patch_jump(cg, ok_jump);
  }

  compile_pattern_bind(cg, pat, acc);
}

// each arm: test the pattern (jumping to the next arm on failure), bind its
// names, run the (optional) guard — false unwinds the binds and falls
// through to the next arm same as a failed test — then the body, sliding
// the arm's locals out from under the result before jumping to the end.
// falling past every arm is a runtime error: the checker doesn't enforce
// match exhaustiveness yet, so this can actually happen.
static void compile_match(Cg *cg, Expr *expr) {
  ExprMatch *match = &expr->as.match;
  int saved_locals_outer = cg->local_count;

  compile_expr(cg, match->subject);
  int subject_slot = cg_add_local(cg, (StringView){0}, expr->span);
  int saved_locals = cg->local_count;
  Accessor subject_acc = {.base_slot = subject_slot};

  JumpList end_jumps = {0};

  for (int i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    // `fail_jumps` (a failed literal/tag test) land with their own test's
    // bool still on the stack — OP_JUMP_IF_FALSE never pops it, only the
    // fallthrough side does — so they route through one shared OP_POP below
    // before falling into `next_arm_jumps`, whose sources (a failed guard)
    // already popped everything themselves and jump straight there.
    JumpList fail_jumps = {0};
    JumpList next_arm_jumps = {0};

    compile_pattern_test(cg, arm->pattern, subject_acc, &fail_jumps);
    compile_pattern_bind(cg, arm->pattern, subject_acc);

    if (arm->guard != NULL) {
      compile_expr(cg, arm->guard);
      int guard_fail_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
      emit(cg, OP_POP);

      compile_expr(cg, arm->body);
      cg_close_scope(cg, saved_locals); // a closure may capture a bound name
      emit_slide(cg, cg->local_count - saved_locals);
      jump_list_push(cg, &end_jumps, arm->span, emit_jump(cg, OP_JUMP));

      patch_jump(cg, guard_fail_jump);
      emit(cg, OP_POP);
      cg_close_scope(cg, saved_locals); // guard could have captured a bind too
      emit2(cg, OP_POPN, (uint8_t)(cg->local_count - saved_locals));
      jump_list_push(cg, &next_arm_jumps, arm->span, emit_jump(cg, OP_JUMP));
    } else {
      compile_expr(cg, arm->body);
      cg_close_scope(cg, saved_locals); // a closure may capture a bound name
      emit_slide(cg, cg->local_count - saved_locals);
      jump_list_push(cg, &end_jumps, arm->span, emit_jump(cg, OP_JUMP));
    }

    jump_list_patch(cg, &fail_jumps);
    emit(cg, OP_POP); // the one outstanding false from whichever test failed
    jump_list_patch(cg, &next_arm_jumps);
    cg->local_count = saved_locals;
  }

  emit(cg, OP_MATCH_FAIL);

  jump_list_patch(cg, &end_jumps);
  emit_slide(cg, 1); // drop the hidden subject local, keep the arm result
  cg->local_count = saved_locals_outer;
}

static void compile_expr_inner(Cg *cg, Expr *expr);

// every expression goes through here, so a coercion recorded by the checker
// cannot be missed by whichever of the ~30 expression cases produced the
// value. The wrap is always the *last* thing: `expr` is compiled as the
// concrete type it is, then boxed.
static void compile_expr(Cg *cg, Expr *expr) {
  compile_expr_inner(cg, expr);
  if (expr->coerce_dyn != NULL) {
    compile_coerce_dyn(cg, expr);
  }
}

static void compile_expr_inner(Cg *cg, Expr *expr) {
  switch (expr->kind) {
  case EXPR_INT:
    emit_const(cg, val_int(expr->as.int_val));
    break;
  case EXPR_FLOAT:
    emit_const(cg, val_float(expr->as.float_val));
    break;
  case EXPR_BOOL:
    emit(cg, expr->as.bool_val ? OP_TRUE : OP_FALSE);
    break;
  case EXPR_UNIT:
    emit(cg, OP_UNIT);
    break;
  case EXPR_STRING:
    emit_const(cg, val_obj(&cg_decode_string(cg, expr->as.string.value)->obj));
    break;
  case EXPR_CHAR:
    // already decoded by the parser, which is the difference from a String:
    // a Char is a value, so it needs no heap object and no intern.
    emit_const(cg, val_char(expr->as.char_val));
    break;

  case EXPR_INTERPOLATED: {
    ExprInterpolated *interp = &expr->as.interpolated;
    for (int i = 0; i < interp->seg_count; i++) {
      InterpolSeg *seg = &interp->segs[i];
      if (seg->kind == ISEG_TEXT) {
        emit_const(cg, val_obj(&cg_decode_string(cg, seg->text)->obj));
      } else {
        compile_expr(cg, seg->expr);
      }
    }
    emit2(cg, OP_INTERP, (uint8_t)interp->seg_count);
    break;
  }

  case EXPR_ARRAY: {
    ExprArray *array = &expr->as.array;
    for (int i = 0; i < array->count; i++) {
      compile_expr(cg, array->elems[i]);
    }
    emit2(cg, OP_ARRAY, (uint8_t)array->count);
    break;
  }

  case EXPR_INDEX: {
    ExprIndex *index = &expr->as.index;
    compile_expr(cg, index->object);
    compile_expr(cg, index->index);
    emit(cg, OP_INDEX_GET);
    break;
  }

  case EXPR_TUPLE: {
    ExprTuple *tuple = &expr->as.tuple;
    for (int i = 0; i < tuple->count; i++) {
      compile_expr(cg, tuple->elems[i]);
    }
    emit2(cg, OP_TUPLE, (uint8_t)tuple->count);
    break;
  }

  case EXPR_STRUCT_INIT:
    compile_struct_init(cg, expr);
    break;

  case EXPR_VARIANT:
    compile_variant_init(cg, expr);
    break;

  case EXPR_FIELD: {
    ExprField *field = &expr->as.field;
    compile_expr(cg, field->object);
    emit2(cg, OP_FIELD_GET, (uint8_t)field->resolved_index);
    break;
  }

  case EXPR_METHOD_CALL:
    compile_method_call(cg, expr);
    break;

  case EXPR_PROPAGATE:
    compile_propagate(cg, expr);
    break;

  case EXPR_SELF: {
    if (cg->self_slot >= 0) {
      emit2(cg, OP_GET_LOCAL, (uint8_t)cg->self_slot);
      break;
    }
    // inside a closure the receiver belongs to the enclosing method's frame,
    // so it is captured like any other local it mentions.
    int upvalue = cg_resolve_upvalue(cg, sv_from_cstr("self"), expr->span);
    if (upvalue < 0) {
      // `self` outside a method is a checker error, so this is unreachable
      // from a program that got here — reported rather than asserted, since a
      // crash is the one thing codegen must not do.
      cg_error(cg, expr->span, "this name");
      emit(cg, OP_UNIT);
      break;
    }
    emit2(cg, OP_GET_UPVALUE, (uint8_t)upvalue);
    break;
  }

  case EXPR_MATCH:
    compile_match(cg, expr);
    break;

  case EXPR_CLOSURE:
    compile_closure(cg, expr);
    break;

  case EXPR_PATH: {
    Path *path = &expr->as.path_expr.path;
    if (path->count != 1) {
      // a multi-segment path expression only ever names an associated
      // function/method reached without dot syntax (e.g. `Point::new`,
      // or `Shape::area(s)` supplying `self` explicitly) — the checker
      // caches which FunDef that resolved to.
      ExprPath *p = &expr->as.path_expr;

      // ...unless the qualifier was a type parameter (`T::from(v)`), where
      // there is no definition to cache: the checker knew only the trait
      // signature, so the body is chosen here, from the substituted self type.
      // This is compile_method_call's bound branch with the receiver moved
      // into the path.
      if (p->bound_trait != NULL) {
        Type *self = subst_apply(&cg->subst, p->bound_self, cg->al);
        Type *trait_ref = subst_apply(&cg->subst, p->bound_trait, cg->al);
        StringView name = path->segments[path->count - 1].name;

        Subst impl_subst = subst_empty();
        FunDef *target =
            cg_bound_target(cg, self, trait_ref, name, &impl_subst, expr->span);
        if (target == NULL) {
          cg_error(cg, expr->span, "this associated function call");
          emit(cg, OP_UNIT);
          break;
        }
        cg_emit_target(cg, target, &p->inst, &impl_subst, expr->span);
        break;
      }

      FunDef *fun = p->resolved_fun;
      if (fun == NULL) {
        cg_error(cg, expr->span, "this path expression");
        emit(cg, OP_UNIT);
        break;
      }
      cg_emit_target(cg, fun, &expr->as.path_expr.inst, NULL, expr->span);
      break;
    }
    StringView name = path->segments[0].name;

    int slot = cg_find_local(cg, name);
    if (slot >= 0) {
      emit2(cg, OP_GET_LOCAL, (uint8_t)slot);
      break;
    }

    int upvalue = cg_resolve_upvalue(cg, name, expr->span);
    if (upvalue >= 0) {
      emit2(cg, OP_GET_UPVALUE, (uint8_t)upvalue);
      break;
    }

    // not a local: a first-class reference to a function, which may live in
    // another module (`use lib::helper; helper()`), possibly under an alias.
    // The checker's answer is on the node — a name search here would miss
    // both cases.
    FunDef *fun = expr->as.path_expr.resolved_fun;
    if (fun != NULL) {
      cg_emit_target(cg, fun, &expr->as.path_expr.inst, NULL, expr->span);
      break;
    }

    cg_error(cg, expr->span, "this name");
    emit(cg, OP_UNIT);
    break;
  }

  case EXPR_BLOCK:
    compile_block(cg, expr);
    break;
  case EXPR_BINARY:
    compile_binary(cg, expr);
    break;
  case EXPR_ASSIGN:
    compile_assign(cg, expr);
    break;
  case EXPR_IF:
    compile_if(cg, expr);
    break;
  case EXPR_WHILE:
    compile_while(cg, expr);
    break;
  case EXPR_FOR:
    compile_for(cg, expr);
    break;
  case EXPR_CALL:
    compile_call(cg, expr);
    break;

  case EXPR_UNARY: {
    compile_expr(cg, expr->as.unary.operand);
    emit(cg, expr->as.unary.op == TOKEN_NOT ? OP_NOT : OP_NEG);
    break;
  }

  case EXPR_RANGE: {
    ExprRange *range = &expr->as.range;
    compile_expr(cg, range->start);
    compile_expr(cg, range->end);
    emit2(cg, OP_RANGE, range->inclusive ? 1 : 0);
    break;
  }

  case EXPR_CAST: {
    ExprCast *cast = &expr->as.cast;
    compile_expr(cg, cast->operand);
    Type *target = cast->target_type->resolved;
    assert(target && "cast target unresolved after checking");
    if (target->kind == TY_FLOAT) {
      emit(cg, OP_CAST_FLOAT);
    } else if (target->kind == TY_INT) {
      emit(cg, OP_CAST_INT);
    } // identity casts: nothing to do
    break;
  }

  case EXPR_POISON:
    assert(false && "poison expr reached codegen");
    break;

  default:
    cg_error(cg, expr->span, "this expression");
    emit(cg, OP_UNIT);
    break;
  }
}

// ── entry ────────────────────────────────────────────────────────────────────

// shared by top-level functions, impl methods and monomorphised instances.
// `fun` owns the chunk being filled in; `body_of` owns the AST (the same
// FunDef except for an instance, which shares its origin's body) and `subst`
// binds the type params that body mentions.
static void compile_fun_body(Mono *mono, FunDef *fun, FunDef *body_of,
                             Subst subst, int depth, ImplIndex *impls,
                             DiagBag *diags, bool *ok) {
  Cg cg = {.m = body_of->module,
           .impls = impls,
           .exe = mono->exe,
           .heap = mono->heap,
           .mono = mono,
           .subst = subst,
           .inst_depth = depth,
           .diags = diags,
           .al = mono->al,
           .ok = true,
           .self_slot = -1};
  cg.chunk = al_alloc_zero_for(mono->al, Chunk);
  chunk_init(cg.chunk, mono->al);

  for (int i = 0; i < fun->param_count; i++) {
    // the receiver is named, not anonymous: a closure in the body captures it
    // by that name like any other local (the parser leaves `self` nameless,
    // and `self` is a keyword, so nothing else can claim the slot).
    bool is_self = fun->params[i].is_self;
    StringView name = is_self ? sv_from_cstr("self") : fun->params[i].name;
    int slot = cg_add_local(&cg, name, body_of->span);
    if (is_self) {
      cg.self_slot = slot;
    }
  }

  compile_expr(&cg, body_of->body);
  emit(&cg, OP_RETURN);

  fun->chunk = cg.chunk;
  *ok &= cg.ok;
}

// a closure expression: compile its body into its own chunk against a child
// compiler chained to the enclosing one (so name lookups resolve captures into
// an upvalue list), then emit OP_CLOSURE to build the runtime ObjClosure from
// the captured cells. the enclosing chunk holds the compiled FunDef as a
// VAL_FUN constant that OP_CLOSURE reads back.
static void compile_closure(Cg *cg, Expr *expr) {
  ExprClosure *closure = &expr->as.closure;
  FunDef *fun = closure->def;
  assert(fun != NULL && "closure not resolved before codegen");

  // A closure inside a *generic* body is compiled once per instantiation, and
  // each copy is a different program: `self.show()` in it resolves through
  // `cg->subst` exactly as it does in the body around it. So it needs its own
  // FunDef, for the same reason `mono_request` copies the enclosing one —
  // `closure->def` is the AST node's, one for every instantiation, and writing
  // a chunk into it lets the last instantiation compiled win for all of them.
  // (The `VAL_FUN` constant the earlier chunks already hold names that shared
  // def, so they would go on to build closures over someone else's body.)
  //
  // A non-generic body has an empty subst and is compiled once, so it keeps
  // the AST's def and pays nothing.
  if (cg->subst.count > 0) {
    FunDef *instance = al_alloc_zero_for(cg->al, FunDef);
    *instance = *fun; // same body, params and name; its own chunk
    instance->chunk = NULL;
    fun = instance;
  }

  Cg child = {.m = cg->m,
              .impls = cg->impls, // as with .subst: the same instantiation
              .exe = cg->exe,
              .heap = cg->heap,
              .mono = cg->mono,
              .subst = cg->subst, // a closure body sees the same type params
              .inst_depth = cg->inst_depth,
              .diags = cg->diags,
              .al = cg->al,
              .ok = true,
              .self_slot = -1,
              .parent = cg};
  child.chunk = al_alloc_zero_for(cg->al, Chunk);
  chunk_init(child.chunk, cg->al);

  for (int i = 0; i < closure->param_count; i++) {
    cg_add_local(&child, closure->params[i].name, expr->span);
  }

  compile_expr(&child, closure->body);
  emit(&child, OP_RETURN);
  fun->chunk = child.chunk;
  cg->ok &= child.ok;

  cg_register_closure(cg, fun);

  int const_idx = cg_add_const(cg, val_fun(fun));
  emit_slot(cg, OP_CLOSURE, const_idx);
  emit(cg, (uint8_t)child.upvalue_count);
  for (int i = 0; i < child.upvalue_count; i++) {
    emit2(cg, child.upvalues[i].is_local ? 1 : 0, child.upvalues[i].index);
  }
}

Module *mono_pending_module(Mono *mono) {
  return mono->compiled < mono->count
             ? mono->insts[mono->compiled].origin->module
             : NULL;
}

bool mono_compile_next(Mono *mono, DiagBag *diags) {
  Instance *it = &mono->insts[mono->compiled++];
  bool ok = true;
  compile_fun_body(mono, it->instance, it->origin, it->subst, it->depth,
                   it->impls, diags, &ok);
  return ok;
}

// ── linking ──────────────────────────────────────────────────────────────────

// one shared complaint for the four slot spaces. No span: outgrowing the
// operand width is a property of the whole program, not of any one
// declaration, so there is nothing honest to point at.
static bool exe_too_many(const char *what, int count) {
  fprintf(stderr,
          "error: the program has %d %s; the VM addresses at most %d "
          "(a two-byte operand)\n",
          count, what, SLOT_MAX);
  return false;
}

// the growth policy for all four tables, stated once. They start empty and
// double, rather than being sized to the operand space up front: since
// milestone 21 a table holds what the program *reaches*, and a typical program
// reaches a few dozen — pre-sizing would now be 64K pointers apiece for a
// ceiling almost nothing comes near.
static int exe_next_cap(int cap) {
  int next = cap == 0 ? 16 : cap * 2;
  return next > SLOT_MAX ? SLOT_MAX : next;
}

// append `fun` to the globals table and record the slot on it. The only
// failure is outgrowing the operand space, which the monomorphiser can hit too
// — it appends here for every instantiation it derives.
static bool exe_add_global(Executable *exe, Allocator *al, FunDef *fun) {
  if (exe->global_count == SLOT_MAX) {
    return exe_too_many("functions and methods", exe->global_count + 1);
  }
  if (exe->global_count == exe->global_cap) {
    int new_cap = exe_next_cap(exe->global_cap);
    exe->globals =
        al_realloc(al, exe->globals, sizeof(FunDef *) * (size_t)exe->global_cap,
                   sizeof(FunDef *) * (size_t)new_cap);
    exe->global_cap = new_cap;
  }
  fun->slot = exe->global_count;
  exe->globals[exe->global_count++] = fun;
  return true;
}

// The one thing about the program's layout that is *not* demand-driven, and
// so all that is left of linking. A variant tag is not a slot: it is an index
// within its own enum, fixed by declaration order and bounded by the variant
// count, and a `match` reads it off an enum the program may never construct —
// so it cannot wait for a reference that might not come.
//
// Everything else that `exe_link` used to do has dissolved into codegen. It
// handed out slots until milestone 21 made them demand-driven, and sized the
// four tables until they learned to grow; with both gone there is no linking
// step distinct from the walk itself.
void exe_assign_tags(ModuleRegistry *reg) {
  for (int i = 0; i < reg->module_count; i++) {
    Module *m = reg->modules[i];
    for (int j = 0; j < m->enum_count; j++) {
      EnumDef *def = m->enums[j];
      for (int k = 0; k < def->variant_count; k++) {
        def->variants[k].tag = (uint8_t)k;
      }
    }
  }
}
