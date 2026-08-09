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

// The two numbers that bound inlining. A body is spliced only if it is under
// CG_INLINE_MAX_COST AST nodes, and a splice may sit inside at most
// CG_INLINE_MAX_DEPTH others — so the code one call site can grow to is
// bounded by the product, not by the program.
#define CG_INLINE_MAX_COST 40
#define CG_INLINE_MAX_DEPTH 2

typedef struct {
  StringView name; // empty for hidden locals
  // The *frame* slot this local occupies, which is where the stack happened to
  // be when it was declared — not its index in `Cg.locals`. The two agree only
  // while nothing else is pending on the stack, and a block is an expression
  // here, so a `var` can be declared with an operand, an argument or a
  // duplicated assignment target already pushed beneath it.
  int slot;
  bool is_captured; // a nested closure captures this slot (needs closing)

  // ── an exploded aggregate ──────────────────────────────────────────────────
  //
  // A local whose object was never built: `field_slots` consecutive slots from
  // `slot` hold its fields, and this entry is the first of that many in
  // `locals` — the ones above it are anonymous, so one entry still means one
  // slot and every scope's pop count stays a difference of `local_count`.
  // `entry` is set only here, and is what a field access matches on: the
  // checker resolved the name, so codegen asks its question rather than a
  // second one about shadowing.
  int field_slots;
  VarEntry *entry;
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

  StringView label; // the name this target declared, or empty

  // a labelled block rather than a loop: no back edge, and invisible to an
  // unlabelled `break`. Its `continue_*` fields are never read — the checker
  // refuses a `continue` that names a block, which is the only way here.
  bool is_block;

  int break_jumps[CG_MAX_BREAKS];
  int break_count;
  int continue_jumps[CG_MAX_BREAKS];
  int continue_count;
  bool continue_is_backward; // while: continue loops back to `start`

  int continue_base; // locals kept on continue (incl. hidden iter locals)
  int break_base;    // locals kept on break (excl. hidden iter locals)
  // The same two points measured in *stack* rather than in locals, which is
  // what the pops actually need: a `break` can be reached with temporaries
  // pushed since the loop began — a callee waiting for its argument, an
  // operand waiting for its partner — and those are on the stack while being
  // no one's local. Counting locals leaves them behind.
  int continue_depth;
  int break_depth;
  // `loop` only: its breaks are its exits, so each leaves the loop's value on
  // the stack and the landing pad needs nothing further. Every other loop
  // shares that pad with the exit it falls out of, which carries no value.
  bool break_takes_value;

  struct CgLoop *parent;
} CgLoop;

// ── threading a match into the constructions that feed it ────────────────────
//
// A `match` over a value every path *builds* need not build it: the site that
// builds it knows the tag, so it knows which arm runs, and can jump into that
// arm with the variant's field where the variant would have been. The fact is
// per path and it is never inferred — each path reports what it pushed as it
// leaves, and a path that has nothing to report is left the tag test it always
// had, at a merge that keeps its old code for exactly that reason.
//
// A continuation is an arm; `CG_THREAD_OTHER` is the one every tag no arm
// names goes to (the next test, the `else`, the loop's exit, the trap).
#define CG_MAX_THREAD_CONTS 7
#define CG_THREAD_OTHER CG_MAX_THREAD_CONTS
#define CG_MAX_THREAD_EXITS 16

typedef struct CgThread {
  // the variant each continuation is entered for
  VariantDef *takes[CG_MAX_THREAD_CONTS];
  int cont_count;

  int jumps[CG_MAX_THREAD_CONTS + 1][CG_MAX_THREAD_EXITS];
  int jump_count[CG_MAX_THREAD_CONTS + 1];
  int taken; // exits routed; zero means nothing was rewritten at all

  // Which splice's `return`s are this subject's exits. Not the first one met —
  // an argument is compiled before the call it belongs to, so the first splice
  // in `match wrap(next())` is `next`'s, whose value is an argument and not the
  // subject at all. `call` is the subject's own call node; the site compiling
  // it arms the claim, and `splice` is the id `cg_inline_body` then took. An id
  // rather than the pad's address, because a dead frame's address is a live
  // frame's address soon enough.
  Expr *call;
  bool armed;
  int splice;
} CgThread;

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
  // in scope bound to a concrete type. Empty for a non-generic definition —
  // where cg_subst still has the projections to collapse, so it is not quite
  // free, but there is nothing to substitute.
  Subst subst;
  int inst_depth; // instantiations traversed to reach this body

  CgLocal locals[CG_MAX_LOCALS];
  int local_count;
  // How many values this frame currently holds — locals *and* the temporaries
  // stacked above them mid-expression. `cg_add_local` needs it because a
  // local's slot is a stack position, and `local_count` stops being one the
  // moment an expression is half-emitted. Kept honest by `compile_expr`, which
  // sets it to "one more than on entry" on the way out whatever the case did,
  // so drift inside a construct cannot outlive that construct.
  int depth;

  CgUpvalue upvalues[CG_MAX_UPVALUES];
  int upvalue_count;

  struct Cg *parent; // enclosing function while compiling a closure; else NULL

  CgLoop *loop;
  bool ok;

  int self_slot; // -1 outside a method; the frame slot holding `self`

  // ── an inlined body, while one is being compiled ───────────────────────────
  //
  // `inline_pad` is the landing pad its `return`s leave through, and it is an
  // ordinary CgLoop because an inlined return *is* a break carrying a value out
  // of a labelled block. `locals_base` is the first entry of `locals` the body
  // may see: everything below belongs to the frame it was spliced into, whose
  // names it must not resolve. `inlining` is the chain of origins currently
  // being spliced, which is what stops a recursive call from expanding forever.
  // All of it is saved and restored around a splice, so nesting works.
  CgLoop *inline_pad;
  int locals_base;
  int inline_depth;
  FunDef *inlining[CG_INLINE_MAX_DEPTH];
  // the splice being compiled, and where the next one's identity comes from.
  // Never reused, so a closed splice cannot be mistaken for a later one.
  int splice_id;
  int splice_seq;

  // ── what the peephole is allowed to look back at ──────────────────────────
  //
  // Three positions in the chunk, each the answer to one question a rewrite
  // that unemits something has to ask. `last_target` is the highest position
  // anything jumps to: code at or below it may be arrived at from elsewhere,
  // so it can only be rewritten, never moved. `last_exit` is the position just
  // past the last unconditional exit, so `last_exit == count` means what comes
  // next is unreachable. `last_get_local` is where the last OP_GET_LOCAL was
  // emitted, which is how `emit_slide` knows the byte before it is an opcode
  // and not some other instruction's operand. `last_enum` is the same trick for
  // the four bytes an OP_ENUM occupies, which is what lets a construction be
  // recognised — and unbuilt — from the merge below it.
  int last_target;
  int last_exit;
  int last_get_local;
  int last_enum;
  EnumDef *last_enum_def;
  uint8_t last_enum_tag;

  // the match the subject being compiled feeds, if any. Saved and restored
  // around a subject, so a nested one cannot steal an outer one's exits.
  CgThread *thread;

  // the body being compiled, which is the scope escape analysis has to search:
  // a declaration asks whether the *rest of this body* ever mentions its name
  // as anything but a field access. Swapped around a splice and a closure, both
  // of which are a different body written somewhere else.
  Expr *body_root;
} Cg;

// Is code emitted here reached? Only if the last instruction was not an
// unconditional exit, or something jumps back to this exact point.
static bool cg_unreachable(const Cg *cg) {
  return cg->last_exit == cg->chunk->count &&
         cg->last_target != cg->chunk->count;
}

// The frame a `break`/`continue` leaves: the innermost *loop* unlabelled, the
// one that declared the name otherwise. Everything the two statements emit is
// already measured *from a frame* — `break_depth`, `continue_depth`, the two
// bases — rather than from the head of the list, so leaving several targets at
// once costs nothing beyond finding the right frame to measure against. The
// checker has refused every name that is not here, so a miss is impossible.
// The block skip has to match `check_loop_target`'s to the letter: two
// resolvers of one name can only be made to agree by asking the same question.
static CgLoop *cg_loop_target(Cg *cg, LoopLabel label) {
  if (label.name.len == 0) {
    for (CgLoop *loop = cg->loop; loop != NULL; loop = loop->parent) {
      if (!loop->is_block) {
        return loop;
      }
    }
    return NULL;
  }
  for (CgLoop *loop = cg->loop; loop != NULL; loop = loop->parent) {
    if (sv_equal(loop->label, label.name)) {
      return loop;
    }
  }
  return NULL;
}

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
//     `int::cmp`/`float::cmp` that `max` happens not to reach.
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

// The queue entry a reached definition came from. It is what the inliner
// needs and a call site cannot carry: the origin owning the body AST, plus the
// three things that body has to be compiled under — its instantiation, its
// impl set, and its depth. The result is only valid until the next request,
// since the queue moves when it grows.
static const Instance *mono_instance_of(Mono *mono, FunDef *instance) {
  for (int i = 0; i < mono->count; i++) {
    if (mono->insts[i].instance == instance) {
      return &mono->insts[i];
    }
  }
  return NULL; // a native: reached and slotted, but with no body to queue
}

// push a type recorded by the checker through the instantiation this body is
// being compiled under. Two steps, and both are needed to reach a concrete
// type: substituting binds the type parameters (`I` → `Counter`), and
// `assoc_apply` then collapses any projection those parameters were the base of
// (`Counter.Item` → `int`) by reading the binding off the impl.
//
// The checker gets the second step from infer_apply, which codegen has no
// equivalent of — leaving it out is what used to make an `I.Item` argument
// arrive at an instantiation still abstract, and be reported as a type argument
// that "is not known here" even though the impl knew it all along.
static Type *cg_subst(Cg *cg, Type *t) {
  return assoc_apply(cg->impls, subst_apply(&cg->subst, t, cg->al), cg->al);
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
  arg = cg_subst(cg, arg);
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
    // `primary` — so `impl<T: Tag> Boxed<int> for T` against `trait Boxed<T>`
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
  cg->last_target = cg->chunk->count; // patching only ever targets here
}

static void emit_loop(Cg *cg, int target_ip) {
  emit(cg, OP_LOOP);
  int dist = cg->chunk->count - target_ip + 2;
  assert(dist >= 0 && dist <= 0xffff && "loop body too large");
  emit2(cg, (uint8_t)((dist >> 8) & 0xff), (uint8_t)(dist & 0xff));
  if (target_ip > cg->last_target) {
    cg->last_target = target_ip; // a back edge is a target like any other
  }
}

// Read a local, and remember where: `emit_slide` below is the one rewrite that
// looks at the instruction it follows, and a chunk cannot be scanned backwards.
static void emit_get_local(Cg *cg, int slot) {
  cg->last_get_local = cg->chunk->count;
  emit2(cg, OP_GET_LOCAL, (uint8_t)slot);
}

// Pop `n` locals' stack slots without disturbing the value on top. Every caller
// means the same n slots: the ones directly beneath the top, so the lowest of
// them is at `cg->depth - 1 - n`.
//
// THE PEEPHOLE. When the value on top is a *copy of the lowest slot the slide
// is about to remove* — `return x` where `x` is the first parameter of a
// spliced body, a block whose tail expression is its own first local — the copy
// and the original are the same value, and the original is already where the
// slide would put it. So the read is unemitted and the slide degrades to
// dropping what was stacked above: one instruction where there were two, or
// none at all when nothing was.
static void emit_slide(Cg *cg, int n) {
  if (n <= 0) {
    return;
  }
  bool folds = cg->last_get_local >= 0 &&
               cg->last_get_local == cg->chunk->count - 2 &&
               cg->last_target != cg->chunk->count &&
               cg->chunk->code[cg->last_get_local + 1] == cg->depth - 1 - n;
  if (folds) {
    cg->chunk->count -= 2; // unemit the read; the original stays put
    cg->last_get_local = -1;
    if (n > 1) {
      emit2(cg, OP_POPN, (uint8_t)(n - 1));
    }
  } else {
    emit2(cg, OP_SLIDE, (uint8_t)n);
  }
  cg->depth -= n;
}

// The variant the last instruction built, or NULL. Same shape as the fold
// above and the same safety argument: an OP_ENUM is four bytes and a chunk
// cannot be read backwards, so the position is remembered, and `last_target`
// is what says no other path arrives between it and here.
static VariantDef *cg_last_variant(const Cg *cg) {
  if (cg->last_enum_def == NULL || cg->last_enum != cg->chunk->count - 4 ||
      cg->last_target == cg->chunk->count) {
    return NULL;
  }
  return &cg->last_enum_def->variants[cg->last_enum_tag];
}

// Route the path that is leaving into the continuation its tag selects, and
// stop building the variant that named it: a one-field one is unemitted
// outright, so the field stays where the instance would have been, and every
// other arity keeps its instance as the placeholder the entry reads through.
// Returns the continuation, or -1 when this path cannot say what it pushed.
//
// Called *before* whatever slide carries the value out, which is why it only
// unemits and does not jump: the caller still owes the value its trip down.
static int cg_thread_claim(Cg *cg, CgThread *th) {
  VariantDef *built = cg_last_variant(cg);
  if (built == NULL) {
    return -1;
  }
  int cont = CG_THREAD_OTHER;
  for (int i = 0; i < th->cont_count; i++) {
    if (th->takes[i] == built) {
      cont = i;
      break;
    }
  }
  if (th->jump_count[cont] >= CG_MAX_THREAD_EXITS) {
    return -1;
  }
  if (built->field_count == 1) {
    cg->chunk->count -= 4; // the field it was about to wrap is the value now
    cg->last_enum_def = NULL;
  }
  th->taken++;
  return cont;
}

static void cg_thread_jump(CgThread *th, int cont, int ip) {
  th->jumps[cont][th->jump_count[cont]++] = ip;
}

// Raw stack traffic, for the bytes a construct emits *around* the expressions
// it contains — a condition popped before the body, a hidden local set up by
// hand. Everything inside one `compile_expr` accounts for itself, so these are
// needed only where the emitted sequence is not itself an expression and
// something after it declares a local or compiles one.
static void cg_pushed(Cg *cg, int n) { cg->depth += n; }

static void cg_popped(Cg *cg, int n) {
  cg->depth -= n;
  assert(cg->depth >= 0 && "stack depth went negative");
}

// ── locals ───────────────────────────────────────────────────────────────────

// Name a value whose slot is known independently of where the stack is now —
// the inliner's case, where every argument is pushed before any of them becomes
// a parameter (a later argument may still name a caller local that a parameter
// would shadow). Returns that *frame slot*, which is what every
// OP_GET_LOCAL/OP_SET_LOCAL wants — never an index into `cg->locals`.
static int cg_add_local_at(Cg *cg, StringView name, int slot, Span span) {
  // Two limits, and the slot is the one that binds: `cg->locals` has room for
  // CG_MAX_LOCALS entries, but what a one-byte operand can address is a
  // *position*, and positions count the temporaries stacked between locals as
  // well. Checking only the count would let a deep expression mint a slot that
  // truncates.
  if (cg->local_count >= CG_MAX_LOCALS || slot >= CG_MAX_LOCALS) {
    diag_error(cg->diags, span, "too many locals in one function");
    cg->ok = false;
    return 0;
  }
  cg->locals[cg->local_count++] = (CgLocal){.name = name, .slot = slot};
  return slot;
}

// Name the value on top of the stack.
static int cg_add_local(Cg *cg, StringView name, Span span) {
  return cg_add_local_at(cg, name, cg->depth - 1, span);
}

// The same, for a value a raw `emit` just pushed rather than one `compile_expr`
// left behind — only the latter is accounted for automatically.
static int cg_add_pushed_local(Cg *cg, StringView name, Span span) {
  cg->depth++;
  return cg_add_local(cg, name, span);
}

// the *index* in `cg->locals`, since callers need both halves: the slot to
// emit, and the entry to mark captured. The search stops at `locals_base`,
// which is 0 outside an inlined body and the first parameter's entry inside
// one: a spliced body was written elsewhere, so a name it does not declare
// itself is a global, never whatever the host frame happens to call the same.
static int cg_find_local(Cg *cg, StringView name) {
  for (int i = cg->local_count - 1; i >= cg->locals_base; i--) {
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
  // The other half of `cg_find_local`'s barrier: a name an inlined body does
  // not declare cannot be a capture either, since the function it was written
  // in is not the one whose frame is around it now.
  if (cg->parent == NULL || cg->inline_pad != NULL) {
    return -1;
  }
  int local = cg_find_local(cg->parent, name);
  if (local >= 0) {
    cg->parent->locals[local].is_captured = true;
    // the capture names a *frame slot* in the parent, not a position in its
    // local list
    return cg_add_upvalue(cg, true, (uint8_t)cg->parent->locals[local].slot,
                          span);
  }
  int upvalue = cg_resolve_upvalue(cg->parent, name, span);
  if (upvalue >= 0) {
    return cg_add_upvalue(cg, false, (uint8_t)upvalue, span);
  }
  return -1;
}

// if any local in [from, local_count) was captured, emit a close so the open
// upvalues over those dying slots copy their values onto the heap. `from` is an
// index into `cg->locals`; the opcode takes the frame slot that index sits at,
// which is the lowest one about to die.
static void cg_close_scope(Cg *cg, int from) {
  for (int i = from; i < cg->local_count; i++) {
    if (cg->locals[i].is_captured) {
      emit2(cg, OP_CLOSE_UPVALUE, (uint8_t)cg->locals[from].slot);
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

// ── one walk over a body, two questions ──────────────────────────────────────
//
// **Which bodies may be spliced.** Compiling a call as the callee's own body in
// the caller's chunk costs one thing at compile time — a second copy of the
// code — and the rule that keeps that bounded is a size cap. So the first
// question is "how big is this body", counted in AST nodes, with `-1` for the
// shapes it will not take at all. The count is also what bounds the *frame*:
// every node can name at most one local and leave at most one temporary live,
// so twice the count bounds the slots a splice adds above the caller's.
//
// **Whether a binding escapes.** An aggregate that never leaves as a whole
// value need not be built — its fields can be slots. `binding` asks that, and
// the answer is *syntactic*: a bare mention is the only way an aggregate can
// reach anywhere else, since there are no borrows to take and no address to
// pass, so `x.f` is the one shape that keeps a value in. Nothing about values
// or dataflow has to be tracked, and the graph `cfg.c` builds is not consulted.
//
// A client sets the fields it reads and ignores the others: `binding == NULL`
// is the size question, and then this walk emits exactly the counts it always
// did.

typedef struct {
  int nodes;
  int returns; // `return` and `?`, which leave through one shared landing pad
  bool ok;

  // the binding under question, NULL for the size client. `escapes` is the
  // pessimistic answer, so every shape this walk does not understand sets it.
  VarEntry *binding;
  bool escapes;
  // inside a closure's body, where even `x.f` escapes: a capture is by slot and
  // an exploded binding is several, so there is no one slot to capture.
  bool in_closure;
} BodyScan;

static void scan_expr(BodyScan *s, Expr *e);

// Is `e` the bare name this scan is asking about?
static bool scan_names_binding(const BodyScan *s, const Expr *e) {
  return s->binding != NULL && e != NULL && e->kind == EXPR_PATH &&
         e->as.path_expr.resolved_local == s->binding;
}

// The same, and reading a field *through* it keeps the value in — which is not
// true inside a closure, where the capture is by slot.
static bool scan_reads_binding(const BodyScan *s, const Expr *e) {
  return !s->in_closure && scan_names_binding(s, e);
}

static void scan_pattern(BodyScan *s, Pattern *p) {
  if (p == NULL) {
    return;
  }
  s->nodes++;
  switch (p->kind) {
  case PAT_WILDCARD:
  case PAT_BIND:
    break;
  case PAT_LITERAL:
    scan_expr(s, p->as.literal_expr);
    break;
  case PAT_VARIANT:
    for (int i = 0; i < p->as.variant.field_count; i++) {
      s->nodes++; // a shorthand field has no sub-pattern and still binds
      scan_pattern(s, p->as.variant.fields[i].sub_pattern);
    }
    break;
  case PAT_STRUCT:
    for (int i = 0; i < p->as.struc.field_count; i++) {
      s->nodes++;
      scan_pattern(s, p->as.struc.fields[i].sub_pattern);
    }
    break;
  case PAT_TUPLE:
    for (int i = 0; i < p->as.tuple.count; i++) {
      scan_pattern(s, p->as.tuple.elems[i]);
    }
    break;
  default:
    s->ok = false;
    s->escapes = true;
  }
}

static void scan_stmt(BodyScan *s, Stmt *st) {
  if (st == NULL) {
    return;
  }
  s->nodes++;
  switch (st->kind) {
  case STMT_EXPR:
    scan_expr(s, st->as.expr_stmt.expr);
    break;
  case STMT_VAR:
    scan_pattern(s, st->as.var_stmt.binding);
    scan_expr(s, st->as.var_stmt.initializer);
    scan_expr(s, st->as.var_stmt.else_block);
    break;
  case STMT_RETURN:
    s->returns++;
    scan_expr(s, st->as.return_stmt.value);
    break;
  case STMT_BREAK:
    scan_expr(s, st->as.break_stmt.value);
    break;
  case STMT_CONTINUE:
    break;
  default:
    s->ok = false;
    s->escapes = true;
  }
}

static void scan_expr(BodyScan *s, Expr *e) {
  if (e == NULL) {
    return;
  }
  s->nodes++;
  switch (e->kind) {
  case EXPR_INT:
  case EXPR_FLOAT:
  case EXPR_BOOL:
  case EXPR_STRING:
  case EXPR_CHAR:
  case EXPR_UNIT:
  case EXPR_SELF:
    break;

  case EXPR_PATH:
    // reached only where the shapes below declined to skip it, so this is the
    // binding used as a whole value: an argument, a subject, a returned thing.
    if (scan_names_binding(s, e)) {
      s->escapes = true;
    }
    break;

  case EXPR_INTERPOLATED:
    for (int i = 0; i < e->as.interpolated.seg_count; i++) {
      s->nodes++;
      scan_expr(s, e->as.interpolated.segs[i].expr);
    }
    break;

  case EXPR_BINARY:
    scan_expr(s, e->as.binary.left);
    scan_expr(s, e->as.binary.right);
    break;
  case EXPR_UNARY:
    scan_expr(s, e->as.unary.operand);
    break;
  case EXPR_ASSIGN:
    // `x.f = v` and `x.f op= v` write a field, which needs no object either.
    // `x = v` does not skip: it is an EXPR_PATH target and escapes below.
    if (e->as.assign.target->kind == EXPR_FIELD &&
        scan_reads_binding(s, e->as.assign.target->as.field.object)) {
      s->nodes += 2; // the field and the name under it, unvisited
    } else {
      scan_expr(s, e->as.assign.target);
    }
    scan_expr(s, e->as.assign.value);
    scan_expr(s, e->as.assign.op_call);
    break;
  case EXPR_RANGE:
    scan_expr(s, e->as.range.start);
    scan_expr(s, e->as.range.end);
    break;

  case EXPR_CALL:
    scan_expr(s, e->as.call.callee);
    for (int i = 0; i < e->as.call.arg_count; i++) {
      scan_expr(s, e->as.call.args[i]);
    }
    break;
  case EXPR_INDEX:
    scan_expr(s, e->as.index.object);
    scan_expr(s, e->as.index.index);
    break;
  case EXPR_FIELD:
    if (scan_reads_binding(s, e->as.field.object)) {
      s->nodes++; // the name under it, which this walk must not call a use
    } else {
      scan_expr(s, e->as.field.object);
    }
    break;
  case EXPR_METHOD_CALL:
    scan_expr(s, e->as.method_call.object);
    for (int i = 0; i < e->as.method_call.arg_count; i++) {
      scan_expr(s, e->as.method_call.args[i]);
    }
    break;
  case EXPR_CAST:
    scan_expr(s, e->as.cast.operand);
    break;
  case EXPR_PROPAGATE:
    s->returns++; // an `Err` leaves by the same door a `return` does
    scan_expr(s, e->as.propagate.operand);
    break;

  case EXPR_BLOCK:
    for (int i = 0; i < e->as.block.stmt_count; i++) {
      scan_stmt(s, e->as.block.stmts[i]);
    }
    scan_expr(s, e->as.block.tail_expr);
    break;
  case EXPR_IF:
    scan_expr(s, e->as.if_expr.condition);
    scan_pattern(s, e->as.if_expr.binding);
    scan_expr(s, e->as.if_expr.then_block);
    scan_expr(s, e->as.if_expr.else_branch);
    break;
  case EXPR_WHILE:
    scan_expr(s, e->as.while_expr.condition);
    scan_pattern(s, e->as.while_expr.binding);
    scan_expr(s, e->as.while_expr.body);
    break;
  case EXPR_LOOP:
    scan_expr(s, e->as.loop_expr.body);
    break;
  case EXPR_FOR:
    scan_expr(s, e->as.for_expr.iterable);
    scan_expr(s, e->as.for_expr.next_call);
    scan_expr(s, e->as.for_expr.body);
    s->nodes += 2; // the hidden iterator and loop variable
    break;
  case EXPR_MATCH:
    scan_expr(s, e->as.match.subject);
    for (int i = 0; i < e->as.match.arm_count; i++) {
      scan_pattern(s, e->as.match.arms[i].pattern);
      scan_expr(s, e->as.match.arms[i].guard);
      scan_expr(s, e->as.match.arms[i].body);
    }
    break;

  case EXPR_TUPLE:
    for (int i = 0; i < e->as.tuple.count; i++) {
      scan_expr(s, e->as.tuple.elems[i]);
    }
    break;
  case EXPR_ARRAY:
    for (int i = 0; i < e->as.array.count; i++) {
      scan_expr(s, e->as.array.elems[i]);
    }
    break;
  case EXPR_STRUCT_INIT:
    for (int i = 0; i < e->as.struct_init.field_count; i++) {
      scan_expr(s, e->as.struct_init.fields[i].value);
    }
    break;
  case EXPR_VARIANT:
    for (int i = 0; i < e->as.variant.field_count; i++) {
      scan_expr(s, e->as.variant.fields[i].value);
    }
    break;

  case EXPR_CLOSURE: {
    // its body is duplicated with the rest, and `compile_closure` gives it a
    // FunDef per splice the way it does per instantiation
    for (int i = 0; i < e->as.closure.param_count; i++) {
      s->nodes++;
    }
    bool outer = s->in_closure;
    s->in_closure = true;
    scan_expr(s, e->as.closure.body);
    s->in_closure = outer;
    break;
  }

  default:
    s->ok = false;
    s->escapes = true; // a shape this walk cannot read is not one to rewrite
  }
}

// How big `fun`'s body is, or -1 for one that may not be spliced at all.
// Memoised on the definition: the answer is a property of the body, and every
// call site to it asks the same question.
static int fun_inline_cost(FunDef *fun) {
  if (fun->inline_cost != 0) {
    return fun->inline_cost;
  }
  BodyScan s = {.ok = fun->body != NULL};
  scan_expr(&s, fun->body);
  bool ok = s.ok && s.nodes <= CG_INLINE_MAX_COST &&
            s.returns < CG_MAX_BREAKS; // the landing pad is one jump list
  fun->inline_cost = ok ? s.nodes : -1;
  return fun->inline_cost;
}

// The body to compile in place of a call to `target`, or NULL to emit the
// call. Everything weighed here is about staying invisible: a splice must fit
// the frame it lands in and be small enough that a second copy is cheap. The
// *depth* cap is what makes a recursive call terminate; the chain check below
// only stops the copies that would then be pointless, since a self-call
// expanded to the cap still ends in a call.
static const Instance *cg_inline_choice(Cg *cg, FunDef *target) {
  if (cg->inline_depth >= CG_INLINE_MAX_DEPTH) {
    return NULL;
  }
  const Instance *inst = mono_instance_of(cg->mono, target);
  if (inst == NULL) {
    return NULL;
  }
  int cost = fun_inline_cost(inst->origin);
  if (cost < 0) {
    return NULL;
  }
  for (int i = 0; i < cg->inline_depth; i++) {
    if (cg->inlining[i] == inst->origin) {
      return NULL; // recursive, directly or through the chain above
    }
  }
  // A frame slot is one byte, and `cg_add_local` turning a working program
  // into "too many locals" would be the one way inlining could be seen.
  if (cg->depth + inst->origin->param_count + 2 * cost > CG_MAX_LOCALS) {
    return NULL;
  }
  return inst;
}

// Leave the function with its value on top of the stack. Inside a spliced body
// there is no frame to drop: the value slides down over the parameters the
// arguments became and jumps to the landing pad. That is milestone 98's
// labelled block to the letter — same frame, same jump list, same slide — so an
// inlined `return` needs no machinery of its own, only the target.
static void cg_emit_return(Cg *cg) {
  CgLoop *pad = cg->inline_pad;
  if (pad == NULL) {
    emit(cg, OP_RETURN);
    cg->last_exit = cg->chunk->count;
    return;
  }
  assert(pad->break_count < CG_MAX_BREAKS && "fun_inline_cost caps the exits");
  // this splice is the subject of a match, and this exit knows which arm it
  // selects: the value stops being a variant and the jump goes to the arm.
  CgThread *th = cg->thread;
  int cont = (th != NULL && th->splice != 0 && th->splice == cg->splice_id)
                 ? cg_thread_claim(cg, th)
                 : -1;

  int n = cg->depth - pad->break_depth - 1;
  if (n > 0) {
    cg_close_scope(cg, pad->break_base);
    emit_slide(cg, n);
  }
  if (cont >= 0) {
    cg_thread_jump(th, cont, emit_jump(cg, OP_JUMP));
  } else {
    pad->break_jumps[pad->break_count++] = emit_jump(cg, OP_JUMP);
  }
  cg->last_exit = cg->chunk->count;
}

// ── expression / statement compilation ───────────────────────────────────────

static void compile_expr(Cg *cg, Expr *expr);
static void compile_closure(Cg *cg, Expr *expr);
static FunDef *cg_bound_target(Cg *cg, Type *self, Type *trait_ref,
                               StringView name, Subst *out_impl_subst,
                               Span span);
static void compile_destructure(Cg *cg, Pattern *pat, int subject_slot,
                                Expr *else_block);
static void cg_thread_merge(Cg *cg, CgThread *th, int subject_slot);
static bool cg_thread_enter(Cg *cg, CgThread *th, int cont, int depth);

// The call about to be spliced is the subject of a threaded match, so the
// splice it opens owns that match's exits. Armed here rather than inside the
// splice because only this site knows *which* call it is compiling: the
// arguments below it have been spliced already.
static void cg_thread_arm(Cg *cg, Expr *call) {
  if (cg->thread != NULL && cg->thread->call == call) {
    cg->thread->armed = true;
  }
}

// ── scalar replacement ───────────────────────────────────────────────────────
//
// A `var` bound to an aggregate *this body builds*, and mentioned nowhere but
// as `x.f`, is compiled as its fields: the construction is never emitted and
// each field is a slot. Both halves are needed and neither implies the other —
// the escape scan says no other name can see the object, and building it here
// says no other name already does, which a bare initializer (`var b = a;`)
// would not.
//
// The declaration mints one `locals` entry per field, the first carrying the
// name and the rest anonymous, so a scope still pops `local_count` slots and no
// other bookkeeping in this file learns about any of it.

// The number of slots to explode this declaration into, or 0 to compile it as
// the object it is written as.
static int cg_explode_arity(Cg *cg, Stmt *stmt) {
  StmtVar *var = &stmt->as.var_stmt;
  if (var->binding->kind != PAT_BIND || var->else_block != NULL ||
      var->binding->as.bind.entry == NULL || cg->body_root == NULL) {
    return 0;
  }
  Expr *init = var->initializer;
  // A coercion wants the object it is about to wrap in a vtable. Only reachable
  // for a `dyn` local nothing ever uses: the one thing a `dyn` can do is take a
  // method call, and that is the receiver leaving as a whole value.
  if (init == NULL || init->coerce_dyn != NULL) {
    return 0;
  }
  int arity;
  if (init->kind == EXPR_TUPLE) {
    arity = init->as.tuple.count;
  } else if (init->kind == EXPR_STRUCT_INIT) {
    arity = init->as.struct_init.resolved_struct->as.struc.def->field_count;
  } else {
    return 0;
  }
  if (arity < 1) {
    return 0; // a fieldless `Empty {}`: the walk below would answer 0 anyway
  }

  BodyScan s = {.binding = var->binding->as.bind.entry};
  scan_expr(&s, cg->body_root);
  return s.escapes ? 0 : arity;
}

// Push an aggregate's fields as themselves — the construction the binding no
// longer needs. Field order is the *definition's*, exactly as
// `compile_struct_init` orders it, so slot k holds the field `resolved_index` k
// names.
static void compile_explode_init(Cg *cg, Expr *init, int arity) {
  if (init->kind == EXPR_TUPLE) {
    for (int i = 0; i < arity; i++) {
      compile_expr(cg, init->as.tuple.elems[i]);
    }
    return;
  }
  ExprStructInit *si = &init->as.struct_init;
  StructDef *def = si->resolved_struct->as.struc.def;
  for (int i = 0; i < arity; i++) {
    compile_expr(cg, find_field_init(si->fields, si->field_count, def->is_tuple,
                                     def->fields[i].ident)
                         ->value);
  }
}

// Name the `arity` values on top of the stack as one binding. A struct only
// ever exploded reaches no slot space at all — `cg_reach_struct` is a
// construction's doing, and there is no longer a construction.
static void cg_add_exploded_local(Cg *cg, Pattern *bind, int arity, Span span) {
  int base = cg->depth - arity;
  cg_add_local_at(cg, bind->as.bind.name, base, span);
  cg->locals[cg->local_count - 1].field_slots = arity;
  cg->locals[cg->local_count - 1].entry = bind->as.bind.entry;
  for (int i = 1; i < arity; i++) {
    cg_add_local_at(cg, (StringView){0}, base + i, span);
  }
}

// The slot holding field `index` of an exploded binding, or -1 when `object` is
// anything else. Matched on the `VarEntry` the checker resolved rather than on
// the name, so the two sides of a shadowed name cannot disagree — which is also
// why the `locals_base` bound `cg_find_local` needs is belt: a binding is one
// entry, so no splice can match an outer body's.
static int cg_exploded_field(Cg *cg, Expr *object, int index) {
  if (object->kind != EXPR_PATH ||
      object->as.path_expr.resolved_local == NULL) {
    return -1;
  }
  VarEntry *ve = object->as.path_expr.resolved_local;
  for (int i = cg->local_count - 1; i >= cg->locals_base; i--) {
    if (cg->locals[i].entry == ve) {
      assert(index < cg->locals[i].field_slots && "field index out of range");
      return cg->locals[i].slot + index;
    }
  }
  return -1;
}

// The statement counterpart of `compile_expr`'s invariant, and the reason no
// case below balances its own stack either: a statement leaves behind exactly
// the locals it declared and nothing else. `var` leaves one (its initializer's
// slot *is* the variable), several when the binding is a pattern or its
// aggregate was exploded; everything else leaves none, and
// `return`/`break`/`continue` leave by another edge entirely, so what the depth
// reads afterwards only has to match the paths that do reach the next
// statement.
static void compile_stmt_inner(Cg *cg, Stmt *stmt);

static void compile_stmt(Cg *cg, Stmt *stmt) {
  int base = cg->depth;
  int base_locals = cg->local_count;
  compile_stmt_inner(cg, stmt);
  cg->depth = base + (cg->local_count - base_locals);
}

static void compile_stmt_inner(Cg *cg, Stmt *stmt) {
  switch (stmt->kind) {
  case STMT_EXPR:
    compile_expr(cg, stmt->as.expr_stmt.expr);
    emit(cg, OP_POP);
    break;

  case STMT_VAR: {
    StmtVar *var = &stmt->as.var_stmt;

    int arity = cg_explode_arity(cg, stmt);
    if (arity > 0) {
      compile_explode_init(cg, var->initializer, arity);
      cg_add_exploded_local(cg, var->binding, arity, stmt->span);
      break;
    }

    compile_expr(cg, var->initializer);

    if (var->binding->kind == PAT_BIND && var->else_block == NULL) {
      // the initializer's stack slot becomes the local
      cg_add_local(cg, var->binding->as.bind.name, stmt->span);
      break;
    }

    // destructuring: the initializer stays put as an anonymous local that the
    // pattern's accessors read from, and the names it binds are appended
    // above it. The temp costs a slot for the rest of the scope, which is
    // what keeps every bound name a plain local like any other.
    int subject_slot = cg_add_local(cg, (StringView){0}, stmt->span);
    compile_destructure(cg, var->binding, subject_slot, var->else_block);
    break;
  }

  case STMT_RETURN: {
    StmtReturn *ret = &stmt->as.return_stmt;
    if (ret->value != NULL) {
      compile_expr(cg, ret->value);
    } else {
      emit(cg, OP_UNIT);
      cg_pushed(cg, 1);
    }
    cg_emit_return(cg);
    break;
  }

  case STMT_BREAK: {
    CgLoop *loop = cg_loop_target(cg, stmt->as.break_stmt.label);
    assert(loop && "break outside loop got past the checker");
    if (loop->break_count >= CG_MAX_BREAKS) {
      diag_error(cg->diags, stmt->span, "too many breaks in one loop");
      cg->ok = false;
      break;
    }
    // the value goes first because it may read the very locals about to go,
    // and OP_SLIDE is what lets it outlive them — the same "remove n beneath
    // the top" a block's tail expression already needed.
    if (loop->break_takes_value) {
      Expr *value = stmt->as.break_stmt.value;
      if (value != NULL) {
        compile_expr(cg, value);
      } else {
        emit(cg, OP_UNIT);
        cg_pushed(cg, 1);
      }
    }
    // everything the loop has stacked since it began, minus the value being
    // carried out past it
    int n = cg->depth - loop->break_depth - (loop->break_takes_value ? 1 : 0);
    if (n > 0) {
      cg_close_scope(cg, loop->break_base); // detach captures before popping
      if (loop->break_takes_value) {
        emit_slide(cg, n);
      } else {
        emit2(cg, OP_POPN, (uint8_t)n);
        cg_popped(cg, n);
      }
    }
    loop->break_jumps[loop->break_count++] = emit_jump(cg, OP_JUMP);
    cg->last_exit = cg->chunk->count;
    break;
  }

  case STMT_CONTINUE: {
    CgLoop *loop = cg_loop_target(cg, stmt->as.continue_stmt.label);
    assert(loop && "continue outside loop got past the checker");
    int n = cg->depth - loop->continue_depth;
    if (n > 0) {
      cg_close_scope(cg, loop->continue_base); // detach captures before popping
      emit2(cg, OP_POPN, (uint8_t)n);
      cg_popped(cg, n);
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
    cg->last_exit = cg->chunk->count;
    break;
  }

  default:
    cg_error(cg, stmt->span, "this statement");
  }
}

// A label costs a block one frame and one round of patching, and nothing else:
// its breaks each arrive having slid their value down to the block's entry
// depth, which is exactly where falling off the end leaves the tail. So the
// landing pad is the block's own exit — there is nothing to emit at it, and no
// jump over anything, because a block has no back edge to jump over.
static void compile_block(Cg *cg, Expr *expr) {
  ExprBlock *block = &expr->as.block;
  int saved_locals = cg->local_count;

  CgLoop frame = {
      .label = block->label.name,
      .is_block = true,
      .break_base = saved_locals,
      .break_depth = cg->depth,
      .break_takes_value = true,
      .parent = cg->loop,
  };
  bool labelled = block->label.name.len > 0;
  if (labelled) {
    cg->loop = &frame;
  }

  for (int i = 0; i < block->stmt_count; i++) {
    compile_stmt(cg, block->stmts[i]);
  }

  // Everything here is the block's exit, and an exit the last statement already
  // took is not reached: a `return`/`break` in tail position left the value
  // where this would have put it. Only the statement-final case is skipped —
  // with a tail expression there is still something to compile, whatever the
  // statements above it did.
  if (block->tail_expr != NULL || !cg_unreachable(cg)) {
    if (block->tail_expr != NULL) {
      compile_expr(cg, block->tail_expr);
    } else {
      emit(cg, OP_UNIT);
    }
    cg_close_scope(cg, saved_locals);
    emit_slide(cg, cg->local_count - saved_locals);
  }
  cg->local_count = saved_locals;

  if (labelled) {
    for (int i = 0; i < frame.break_count; i++) {
      patch_jump(cg, frame.break_jumps[i]);
    }
    cg->loop = frame.parent;
  }
}

// The opcode a binary operator emits, for the operators that are one — `and`
// and `or` short-circuit and so are control flow rather than an opcode. False
// for anything else. A compound assignment reads this map too, through
// `token_compound_binary_op`, which is what makes `a op= b` and `a op b`
// unable to emit different instructions.
static bool binary_opcode(TokenType op, OpCode *out) {
  switch (op) {
  case TOKEN_PLUS:
    *out = OP_ADD;
    return true;
  case TOKEN_MINUS:
    *out = OP_SUB;
    return true;
  case TOKEN_STAR:
    *out = OP_MUL;
    return true;
  case TOKEN_SLASH:
    *out = OP_DIV;
    return true;
  case TOKEN_PERCENT:
    *out = OP_MOD;
    return true;
  case TOKEN_AMP:
    *out = OP_BIT_AND;
    return true;
  case TOKEN_PIPE:
    *out = OP_BIT_OR;
    return true;
  case TOKEN_CARET:
    *out = OP_BIT_XOR;
    return true;
  case TOKEN_SHL:
    *out = OP_SHL;
    return true;
  case TOKEN_SHR:
    *out = OP_SHR;
    return true;
  case TOKEN_USHR:
    *out = OP_USHR;
    return true;
  case TOKEN_EQEQ:
    *out = OP_EQ;
    return true;
  case TOKEN_BANGEQ:
    *out = OP_NEQ;
    return true;
  case TOKEN_LT:
    *out = OP_LT;
    return true;
  case TOKEN_LTEQ:
    *out = OP_LTEQ;
    return true;
  case TOKEN_GT:
    *out = OP_GT;
    return true;
  case TOKEN_GTEQ:
    *out = OP_GTEQ;
    return true;
  default:
    return false;
  }
}

static void compile_binary(Cg *cg, Expr *expr) {
  ExprBinary *binary = &expr->as.binary;
  TokenType op = binary->op;

  // short-circuit logic: the left operand is popped only on the path that goes
  // on to evaluate the right one, so the right is compiled one value shallower
  // than the jump it hangs off.
  if (op == TOKEN_AND) {
    compile_expr(cg, binary->left);
    int end = emit_jump(cg, OP_JUMP_IF_FALSE);
    emit(cg, OP_POP);
    cg_popped(cg, 1);
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
    cg_popped(cg, 1);
    compile_expr(cg, binary->right);
    patch_jump(cg, end);
    return;
  }

  compile_expr(cg, binary->left);
  compile_expr(cg, binary->right);

  OpCode code;
  if (!binary_opcode(op, &code)) {
    cg_error(cg, expr->span, "this operator");
    return;
  }
  emit(cg, code);
}

static bool cg_emit_target(Cg *cg, FunDef *fun, const Subst *primary,
                           const Subst *fallback, Span span);

// What a compound assignment's operator compiles to. `fun == NULL` is the
// built-in case — an opcode over two numbers or two strings — and anything else
// is the `a.add(b)` the checker parked on `ExprAssign.op_call`.
typedef struct {
  FunDef *fun;
  Subst impl_subst;
  Expr *call;
} CgCompound;

// The half of a compound assignment that has to happen *before* the place's
// current value is read: OP_CALL wants [callee, self, arg] and that current
// value is the `self`, so a trait operator's callee goes underneath it. The
// built-in case pushes nothing, which is why the DUP depths below shift by one
// exactly when `fun` is set. Returns false after reporting.
static bool cg_compound_begin(Cg *cg, Expr *expr, CgCompound *out) {
  *out = (CgCompound){.impl_subst = subst_empty()};

  Expr *call = expr->as.assign.op_call;
  if (call == NULL) {
    return true; // an opcode; nothing to push
  }

  ExprMethodCall *mc = &call->as.method_call;
  if (mc->resolved_method != NULL) {
    out->fun = mc->resolved_method->fun;
  } else if (mc->resolved_default != NULL) {
    out->fun = mc->resolved_default;
  } else if (mc->bound_trait != NULL) {
    Type *self = cg_subst(cg, mc->bound_self);
    if (self->kind == TY_DYN) {
      // unreachable in practice: every `std::ops` method returns `Self.Output`
      // or takes a `Self`, so none is dispatchable and no trait object of one
      // exists to be assigned into.
      cg_error(cg, expr->span,
               "this compound assignment through a trait "
               "object");
      return false;
    }
    out->fun = cg_bound_target(cg, self, cg_subst(cg, mc->bound_trait),
                               mc->method_name, &out->impl_subst, expr->span);
  }

  if (out->fun == NULL) {
    cg_error(cg, expr->span, "this compound assignment");
    return false;
  }
  // the stack shape below is [.., self, arg], so the body must want exactly
  // that: an `@intrinsic` (which is an opcode with no slot to address) or a
  // receiver anywhere but first would need a different one.
  if (out->fun->native_kind == ATTR_INTRINSIC || out->fun->param_count != 2 ||
      !out->fun->params[0].is_self) {
    cg_error(cg, expr->span, "this compound assignment");
    return false;
  }

  out->call = call;
  return cg_emit_target(cg, out->fun, &mc->inst, &out->impl_subst, expr->span);
}

// ...and the half that runs with [.., self, arg] on top: the operator itself,
// which is the *binary* one — `a op= b` is `a = a op b` down here too, so the
// opcode comes from the same map `a op b` reads.
static void cg_compound_end(Cg *cg, Expr *expr, const CgCompound *c) {
  if (c->fun != NULL) {
    emit2(cg, OP_CALL, 2);
    return;
  }
  OpCode code;
  if (!binary_opcode(token_compound_binary_op(expr->as.assign.op), &code)) {
    cg_error(cg, expr->span, "this compound assignment");
    return;
  }
  emit(cg, code);
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
    CgCompound c;
    if (!cg_compound_begin(cg, expr, &c)) { // [array, index, callee?]
      return;
    }
    uint8_t d = c.fun != NULL ? 2 : 1; // the callee sits between, if pushed
    emit2(cg, OP_DUP, d);              // [.., array]
    emit2(cg, OP_DUP, d);              // [.., array, index]
    emit(cg, OP_INDEX_GET);            // [.., current]
    cg_pushed(cg, 1);                  // the two dups, read down to one
    compile_expr(cg, assign->value);   // [.., current, value]
    cg_compound_end(cg, expr, &c);     // [array, index, combined]
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

  // an exploded binding's field *is* a local, so this is `x op= v` on a slot —
  // no object beneath, and OP_SET_LOCAL leaves the value the way OP_FIELD_SET
  // does.
  int slot = cg_exploded_field(cg, field->object, field->resolved_index);
  if (slot >= 0) {
    if (assign->op == TOKEN_EQ) {
      compile_expr(cg, assign->value);
    } else {
      CgCompound c;
      if (!cg_compound_begin(cg, expr, &c)) { // [callee?]
        return;
      }
      emit_get_local(cg, slot); // [.., current]
      cg_pushed(cg, 1);
      compile_expr(cg, assign->value); // [.., current, value]
      cg_compound_end(cg, expr, &c);   // [combined]
    }
    emit2(cg, OP_SET_LOCAL, (uint8_t)slot);
    return;
  }

  compile_expr(cg, field->object); // [obj]

  if (assign->op == TOKEN_EQ) {
    compile_expr(cg, assign->value); // [obj, value]
  } else {
    CgCompound c;
    if (!cg_compound_begin(cg, expr, &c)) { // [obj, callee?]
      return;
    }
    emit2(cg, OP_DUP, c.fun != NULL ? 1 : 0); // [.., obj]
    emit2(cg, OP_FIELD_GET, idx);             // [.., current]
    cg_pushed(cg, 1);                         // the dup, read into the field
    compile_expr(cg, assign->value);          // [.., current, value]
    cg_compound_end(cg, expr, &c);            // [obj, combined]
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
  int local = cg_find_local(cg, name);
  uint8_t get_op, set_op, operand;
  if (local >= 0) {
    get_op = OP_GET_LOCAL;
    set_op = OP_SET_LOCAL;
    operand = (uint8_t)cg->locals[local].slot;
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
    CgCompound c;
    if (!cg_compound_begin(cg, expr, &c)) { // [callee?]
      return;
    }
    emit2(cg, get_op, operand); // [.., current]
    cg_pushed(cg, 1);
    compile_expr(cg, assign->value); // [.., current, value]
    cg_compound_end(cg, expr, &c);   // [combined]
  }

  emit2(cg, set_op, operand); // value stays as the expr result
}

static void compile_if_binding(Cg *cg, Expr *expr);
static void compile_while_binding(Cg *cg, Expr *expr);

static void compile_if(Cg *cg, Expr *expr) {
  ExprIf *if_ = &expr->as.if_expr;

  if (if_->binding != NULL) {
    compile_if_binding(cg, expr);
    return;
  }

  compile_expr(cg, if_->condition);
  int else_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  // a branch target resumes the stack its *fork* had, not the one the path
  // falling into it ended with — the two arms are alternatives, so the depth
  // has to be put back rather than carried across.
  int forked = cg->depth; // the condition is still live at the fork
  emit(cg, OP_POP);
  cg_popped(cg, 1);
  compile_expr(cg, if_->then_block);
  int end_jump = emit_jump(cg, OP_JUMP);

  patch_jump(cg, else_jump);
  cg->depth = forked;
  emit(cg, OP_POP);
  cg_popped(cg, 1);
  if (if_->else_branch != NULL) {
    compile_expr(cg, if_->else_branch);
  } else {
    emit(cg, OP_UNIT);
  }
  patch_jump(cg, end_jump);
}

static void compile_while(Cg *cg, Expr *expr) {
  ExprWhile *wh = &expr->as.while_expr;

  if (wh->binding != NULL) {
    compile_while_binding(cg, expr);
    return;
  }

  CgLoop loop = {
      .label = expr->as.while_expr.label.name,
      .start = cg->chunk->count,
      .continue_is_backward = true,
      .continue_base = cg->local_count,
      .break_base = cg->local_count,
      .continue_depth = cg->depth,
      .break_depth = cg->depth,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  compile_expr(cg, wh->condition);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);
  cg_popped(cg, 1); // the condition, before the body may declare anything

  compile_expr(cg, wh->body);
  emit(cg, OP_POP);
  cg_popped(cg, 1);
  emit_loop(cg, loop.start);

  patch_jump(cg, exit_jump);
  emit(cg, OP_POP); // the condition
  for (int i = 0; i < loop.break_count; i++) {
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

// `loop { .. }` is `compile_while` with the condition and its exit jump taken
// out — the only way past the back-edge is a patched `break`, and each one of
// those brings the loop's value with it. So there is nothing to emit at the
// landing pad; the trailing `OP_UNIT` is only for the loop nothing leaves,
// where it is unreachable but keeps every caller's "an expression leaves a
// value" invariant true for one byte.
static void compile_loop(Cg *cg, Expr *expr) {
  CgLoop loop = {
      .label = expr->as.loop_expr.label.name,
      .start = cg->chunk->count,
      .continue_is_backward = true,
      .continue_base = cg->local_count,
      .break_base = cg->local_count,
      .continue_depth = cg->depth,
      .break_depth = cg->depth,
      .break_takes_value = true,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  compile_expr(cg, expr->as.loop_expr.body);
  emit(cg, OP_POP);
  cg_popped(cg, 1);
  emit_loop(cg, loop.start);

  for (int i = 0; i < loop.break_count; i++) {
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  if (loop.break_count == 0) {
    emit(cg, OP_UNIT);
  }
}

static void compile_for_range(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  int saved_locals = cg->local_count;
  int entry_depth = cg->depth;

  // [range, i] as hidden + loop locals
  compile_expr(cg, for_->iterable);
  int range_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit_get_local(cg, range_slot);
  emit(cg, OP_RANGE_START);
  int i_slot = cg_add_pushed_local(cg, for_->var_name, for_->var_span);
  // the loop-carried slots are all the body ever sees beneath it; the test and
  // the read that run in between balance out, but saying so beats trusting it
  int body_depth = cg->depth;

  CgLoop loop = {
      .label = expr->as.for_expr.label.name,
      .continue_is_backward = false, // continue jumps forward to the increment
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .continue_depth = cg->depth,
      .break_depth = entry_depth,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;
  emit_get_local(cg, range_slot);
  emit_get_local(cg, i_slot);
  emit(cg, OP_RANGE_TEST);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);

  cg->depth = body_depth;
  compile_expr(cg, for_->body);
  emit(cg, OP_POP);
  cg_popped(cg, 1);

  // increment; forward `continue`s land here
  for (int i = 0; i < loop.continue_count; i++) {
    patch_jump(cg, loop.continue_jumps[i]);
  }
  emit_get_local(cg, i_slot);
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
  int entry_depth = cg->depth;

  // [arr, idx, var] as hidden + loop locals; var is a placeholder (unit)
  // until the first bounds-checked read.
  compile_expr(cg, for_->iterable);
  int arr_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit_const(cg, val_int(0));
  int idx_slot = cg_add_pushed_local(cg, (StringView){0}, expr->span);
  emit(cg, OP_UNIT);
  int var_slot = cg_add_pushed_local(cg, for_->var_name, for_->var_span);
  int body_depth = cg->depth;

  CgLoop loop = {
      .label = expr->as.for_expr.label.name,
      .continue_is_backward = false, // continue jumps forward to the increment
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .continue_depth = cg->depth,
      .break_depth = entry_depth,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;
  emit_get_local(cg, idx_slot);
  emit_get_local(cg, arr_slot);
  emit(cg, OP_LEN);
  emit(cg, OP_LT);
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE);
  emit(cg, OP_POP);

  emit_get_local(cg, arr_slot);
  emit_get_local(cg, idx_slot);
  emit(cg, OP_INDEX_GET);
  emit2(cg, OP_SET_LOCAL, (uint8_t)var_slot);
  emit(cg, OP_POP);

  cg->depth = body_depth;
  compile_expr(cg, for_->body);
  emit(cg, OP_POP);
  cg_popped(cg, 1);

  // increment; forward `continue`s land here
  for (int i = 0; i < loop.continue_count; i++) {
    patch_jump(cg, loop.continue_jumps[i]);
  }
  emit_get_local(cg, idx_slot);
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
static void cg_inline_body(Cg *cg, const Instance *inst, int base_depth,
                           Span span);

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

  // Which `next` to call — the same three shapes the checker leaves on any
  // method call (compile_method_call): a concrete impl method, an inherited
  // default, or dispatch through a bound. The last is what a `for` over a
  // generic `I: Iterator` (or a `dyn Iterator`) needs — the checker knew the
  // receiver only abstractly, so codegen makes it concrete and re-selects the
  // body, exactly as monomorphisation does for a bounded method call.
  FunDef *next = NULL;
  Subst impl_subst = subst_empty();
  int dyn_index = -1; // >= 0 when the receiver is a trait object
  if (mc->resolved_method != NULL) {
    next = mc->resolved_method->fun;
  } else if (mc->resolved_default != NULL) {
    next = mc->resolved_default;
  } else if (mc->bound_trait != NULL) {
    Type *self = cg_subst(cg, mc->bound_self);
    if (self->kind == TY_DYN) {
      // a `dyn Iterator`: the receiver carries the vtable, so the slot is all
      // codegen picks — `next`'s position in the trait's method list, read
      // over the supertrait closure so a `dyn DoubleEnded` finds the `next`
      // its super declares (the same numbering cg_vtable_for built with).
      dyn_index = trait_flat_method_index(dyn_trait_def(self), mc->method_name);
    } else {
      Type *trait_ref = cg_subst(cg, mc->bound_trait);
      next = cg_bound_target(cg, self, trait_ref, mc->method_name, &impl_subst,
                             expr->span);
    }
  }
  if (next == NULL && dyn_index < 0) {
    cg_error(cg, expr->span, "this iterator");
    emit(cg, OP_UNIT);
    return;
  }

  int saved_locals = cg->local_count;
  int entry_depth = cg->depth;

  // [iter, var]: `iter` is the receiver, whose cursor advances in place; `var`
  // is a placeholder (unit) until each `Some` binds it, reused every turn.
  compile_expr(cg, for_->iterable);
  int iter_slot = cg_add_local(cg, (StringView){0}, expr->span);
  emit(cg, OP_UNIT);
  int var_slot = cg_add_pushed_local(cg, for_->var_name, for_->var_span);
  int body_depth = cg->depth;

  CgLoop loop = {
      .label = expr->as.for_expr.label.name,
      .continue_is_backward = true, // `continue` re-drives next()
      .continue_base = cg->local_count,
      .break_base = saved_locals,
      .continue_depth = cg->depth,
      .break_depth = entry_depth,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  int loop_start = cg->chunk->count;
  loop.start = loop_start;

  // `Some` binds the loop variable and runs the body; every other tag is the
  // exit. Both are continuations a `next()` that constructs its answer can jump
  // to directly — see "Threading a match into its constructions".
  CgThread th = {0};
  CgThread *saved_thread = cg->thread;
  th.takes[0] = for_->some_variant;
  th.cont_count = 1;
  cg->thread = &th;

  // opt = iter.next() — the receiver is the hidden local, not a re-eval, so its
  // cursor advances across turns rather than restarting.
  if (dyn_index >= 0) {
    // OP_DYN_METHOD pops the trait object and leaves [next, receiver], the
    // shape OP_CALL already understands; `next` takes only `self`.
    emit_get_local(cg, iter_slot);                // [iter]
    emit2(cg, OP_DYN_METHOD, (uint8_t)dyn_index); // [next, recv]
    emit2(cg, OP_CALL, 1);                        // [opt]
  } else {
    FunDef *target =
        cg_call_target(cg, next, &mc->inst, &impl_subst, expr->span);
    if (target == NULL) {
      cg->thread = saved_thread; // `th` is about to go out of scope
      cg->loop = loop.parent;
      cg->local_count = saved_locals;
      emit(cg, OP_UNIT);
      return;
    }
    const Instance *found = cg_inline_choice(cg, target);
    if (found != NULL) {
      // the whole point of inlining: the `Some` this mints and the tag test
      // below it end up in one chunk, with no frame between them — and once
      // they are, neither has to happen at all
      Instance inst = *found;
      emit_get_local(cg, iter_slot); // [iter]
      cg_pushed(cg, 1);
      th.armed = true; // this call *is* the subject; there is no other
      cg_inline_body(cg, &inst, body_depth, expr->span); // [opt]
    } else {
      cg_pushed(cg, 1);
      emit_slot(cg, OP_GET_GLOBAL, target->slot);
      emit_get_local(cg, iter_slot);                  // [next, iter]
      emit2(cg, OP_CALL, (uint8_t)next->param_count); // [opt]
    }
  }
  cg->thread = saved_thread;

  if (th.taken > 0) {
    cg->depth = body_depth + 1; // the value `next()` left, in its own slot
    cg_thread_merge(cg, &th, body_depth);

    cg_thread_enter(cg, &th, 0, body_depth + 1);
    emit2(cg, OP_SET_LOCAL, (uint8_t)var_slot); // var = the payload itself
    emit(cg, OP_POP);
    cg->depth = body_depth;
    compile_expr(cg, for_->body);
    emit(cg, OP_POP);
    cg_popped(cg, 1);
    emit_loop(cg, loop_start);

    cg_thread_enter(cg, &th, CG_THREAD_OTHER, body_depth + 1);
    emit(cg, OP_POP); // the placeholder no arm read
    cg_popped(cg, 1);
    cg_close_scope(cg, saved_locals); // detach a captured loop var
    emit2(cg, OP_POPN, 2);            // var, iter
    for (int i = 0; i < loop.break_count; i++) {
      patch_jump(cg, loop.break_jumps[i]);
    }
    cg->loop = loop.parent;
    cg->local_count = saved_locals;
    emit(cg, OP_UNIT); // loops evaluate to unit
    return;
  }

  // Some -> bind and run the body; None -> exit
  emit2(cg, OP_DUP, 0); // [opt, opt]
  emit(cg, OP_TAG);     // [opt, tag]
  emit_const(cg, val_int(for_->some_variant->tag));
  emit(cg, OP_EQ);                                 // [opt, is_some]
  int exit_jump = emit_jump(cg, OP_JUMP_IF_FALSE); // peeks is_some
  emit(cg, OP_POP);                                // is_some (true) -> [opt]
  emit2(cg, OP_FIELD_GET, 0);                      // [payload] (Some.0)
  emit2(cg, OP_SET_LOCAL, (uint8_t)var_slot);      // var = payload; [payload]
  emit(cg, OP_POP);                                // []

  cg->depth = body_depth;
  compile_expr(cg, for_->body);
  emit(cg, OP_POP);
  cg_popped(cg, 1);
  emit_loop(cg, loop_start);

  patch_jump(cg, exit_jump);                   // [opt, is_some]
  emit(cg, OP_POP);                            // is_some -> [opt]
  emit(cg, OP_POP);                            // the None instance -> []
  cg_close_scope(cg, saved_locals);            // detach a captured loop var
  emit2(cg, OP_POPN, 2);                       // var, iter
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
// Pushes the callee — one value either way, which is why the accounting lives
// here rather than at the seven call sites: a callee is not an expression node,
// so nothing else would have counted it, and the arguments compiled on top of
// it are exactly where a block declaring a local can land.
static bool cg_emit_target(Cg *cg, FunDef *fun, const Subst *primary,
                           const Subst *fallback, Span span) {
  FunDef *target = cg_call_target(cg, fun, primary, fallback, span);
  cg_pushed(cg, 1);
  if (target == NULL) {
    emit(cg, OP_UNIT);
    return false;
  }
  emit_slot(cg, OP_GET_GLOBAL, target->slot);
  return true;
}

// Compile `inst`'s body where the call to it would have gone. The arguments
// are already on the stack, at `base_depth` and up — and that is the whole of
// the renumbering an inliner usually owes, because milestone 88 made a local's
// slot *a stack position*: an argument pushed at the call site already sits
// where the parameter goes, so naming it is the entire prologue and no
// instruction moves.
//
// Everything the body may see is swapped for the definition's own — its
// instantiation, its impl set, its locals, and no loop of the caller's for a
// `break` to reach out to. The value it leaves replaces the arguments, which is
// what an OP_CALL would have done.
static void cg_inline_body(Cg *cg, const Instance *inst, int base_depth,
                           Span span) {
  FunDef *origin = inst->origin;
  int saved_locals = cg->local_count;
  int self_slot = -1;

  for (int i = 0; i < origin->param_count; i++) {
    bool is_self = origin->params[i].is_self;
    StringView name = is_self ? sv_from_cstr("self") : origin->params[i].name;
    int slot = cg_add_local_at(cg, name, base_depth + i, span);
    if (is_self) {
      self_slot = slot;
    }
  }

  CgLoop pad = {.is_block = true,
                .break_takes_value = true,
                .break_base = saved_locals,
                .break_depth = base_depth};

  Module *saved_m = cg->m;
  ImplIndex *saved_impls = cg->impls;
  Subst saved_subst = cg->subst;
  int saved_inst_depth = cg->inst_depth;
  int saved_self_slot = cg->self_slot;
  CgLoop *saved_loop = cg->loop;
  CgLoop *saved_pad = cg->inline_pad;
  int saved_base = cg->locals_base;
  int saved_splice = cg->splice_id;
  Expr *saved_root = cg->body_root;

  cg->splice_id = ++cg->splice_seq;
  // the site that compiled the call armed this if its value is the subject of
  // a threaded match; the returns below are then that match's paths.
  if (cg->thread != NULL && cg->thread->armed) {
    cg->thread->armed = false;
    cg->thread->splice = cg->splice_id;
  }
  cg->m = origin->module;
  cg->impls = inst->impls;
  cg->subst = inst->subst;
  cg->inst_depth = inst->depth;
  cg->self_slot = self_slot;
  cg->loop = NULL;
  cg->inline_pad = &pad;
  cg->locals_base = saved_locals;
  cg->body_root = origin->body;
  cg->inlining[cg->inline_depth++] = origin;

  compile_expr(cg, origin->body);

  // Falling off the end is the last exit, and it lands where every `return`
  // slid its value to — so the landing pad is the body's own exit, with
  // nothing to emit at it.
  //
  // Unless the body *ended* in a `return`: then the jump it left through is the
  // last instruction, its target is the byte after it, and the fall-through
  // this would emit is code no path reaches. Unemitting the jump costs the pad
  // nothing — the exit arrives by falling into it instead.
  if (pad.break_count > 0 && cg_unreachable(cg) &&
      pad.break_jumps[pad.break_count - 1] + 2 == cg->chunk->count) {
    cg->chunk->count -= 3;
    pad.break_count--;
    cg->last_exit = -1; // reached by fall-through now
  } else if (!cg_unreachable(cg)) {
    cg_close_scope(cg, saved_locals);
    emit_slide(cg, cg->depth - base_depth - 1);
  }
  for (int i = 0; i < pad.break_count; i++) {
    patch_jump(cg, pad.break_jumps[i]);
  }
  cg->inline_depth--;
  cg->splice_id = saved_splice;
  cg->m = saved_m;
  cg->impls = saved_impls;
  cg->subst = saved_subst;
  cg->inst_depth = saved_inst_depth;
  cg->self_slot = saved_self_slot;
  cg->loop = saved_loop;
  cg->inline_pad = saved_pad;
  cg->locals_base = saved_base;
  cg->body_root = saved_root;
  cg->local_count = saved_locals;
  cg->depth = base_depth + 1;
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

  // A call to a definition *by name* is the one shape a body can be spliced
  // into: the target is settled here rather than by whatever the callee
  // expression evaluates to, and the arguments are its parameters in order.
  if (direct != NULL && !fun_is_native(direct) &&
      direct->param_count == call->arg_count) {
    FunDef *target = cg_call_target(cg, direct, &callee->as.path_expr.inst,
                                    NULL, callee->span);
    if (target == NULL) {
      emit(cg, OP_UNIT); // reported; the program will not be run
      return;
    }
    const Instance *found = cg_inline_choice(cg, target);
    if (found != NULL) {
      // by value: compiling the body enqueues its own callees, and the queue
      // moves when it grows
      Instance inst = *found;
      int base_depth = cg->depth;
      for (int i = 0; i < call->arg_count; i++) {
        compile_expr(cg, call->args[i]);
      }
      cg_thread_arm(cg, expr);
      cg_inline_body(cg, &inst, base_depth, expr->span);
      return;
    }
    cg_pushed(cg, 1);
    emit_slot(cg, OP_GET_GLOBAL, target->slot);
    for (int i = 0; i < call->arg_count; i++) {
      compile_expr(cg, call->args[i]);
    }
    emit2(cg, OP_CALL, (uint8_t)call->arg_count);
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

// ── upcasting a trait object ─────────────────────────────────────────────────
//
// `dyn Sub` -> `dyn Super` swaps tables, and the table to swap to depends on
// the concrete type. The *site* knows the two traits and not the type; the
// table the value carries knows the type and was built for one of the traits.
// So the link is stored on the source table, and these two functions are the
// two halves of computing every one of them:
//
//   - a new pair has to catch up with the tables that already exist
//   - a new table has to catch up with the pairs already recorded
//
// Doing both incrementally is the fixpoint, so nothing has to run after the
// worklist drains — which matters because building a target table can queue
// bodies, and a pass that ran *after* the drain would have to restart it.
static int cg_vtable_for(Cg *cg, Type *trait_ref, Type *self, Span span);

// give `mono->vtables[vi]` its link for `pair`, building the target table if
// this is the first concrete type to need one.
static void cg_link_upcast(Cg *cg, int vi, int pair, Span span) {
  Mono *mono = cg->mono;
  VTable *from_vt = mono->exe->vtables[mono->vtables[vi].index];
  for (int i = 0; i < from_vt->upcast_count; i++) {
    if (from_vt->upcasts[i].pair == pair) {
      return; // already linked
    }
  }

  // read the key out before the recursion: cg_vtable_for may realloc both
  // tables it appends to, so nothing may be held across it but an index.
  Type *self = mono->vtables[vi].self_type;
  int ti = cg_vtable_for(cg, mono->upcasts[pair].to, self, span);
  if (ti < 0) {
    return; // reported
  }
  from_vt = mono->exe->vtables[mono->vtables[vi].index];

  int n = from_vt->upcast_count;
  from_vt->upcasts =
      al_realloc(cg->al, from_vt->upcasts, sizeof(VTableUpcast) * (size_t)n,
                 sizeof(VTableUpcast) * (size_t)(n + 1));
  from_vt->upcasts[n] =
      (VTableUpcast){.pair = (uint16_t)pair, .to = mono->exe->vtables[ti]};
  from_vt->upcast_count = n + 1;
}

// record (and dedupe) an upcast spelling, returning the `pair` id its opcode
// carries.
static int cg_upcast_pair(Cg *cg, Type *from, Type *to, Span span) {
  Mono *mono = cg->mono;
  for (int i = 0; i < mono->upcast_count; i++) {
    if (mono->upcasts[i].from == from && mono->upcasts[i].to == to) {
      return i;
    }
  }
  if (mono->upcast_count == SLOT_MAX) {
    exe_too_many("trait-object upcasts", mono->upcast_count + 1);
    cg->ok = false;
    return -1;
  }
  if (mono->upcast_count == mono->upcast_cap) {
    int new_cap = mono->upcast_cap == 0 ? 4 : mono->upcast_cap * 2;
    mono->upcasts = al_realloc(mono->al, mono->upcasts,
                               sizeof(UpcastPair) * (size_t)mono->upcast_cap,
                               sizeof(UpcastPair) * (size_t)new_cap);
    mono->upcast_cap = new_cap;
  }
  int pair = mono->upcast_count++;
  mono->upcasts[pair] = (UpcastPair){.from = from, .to = to};

  // the count is re-read rather than snapshotted: linking appends the target
  // tables, and a fresh one may be the source of a pair of its own.
  for (int i = 0; i < mono->vtable_count; i++) {
    if (mono->vtables[i].trait == from) {
      cg_link_upcast(cg, i, pair, span);
    }
  }
  return pair;
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
  // laid out over the trait's *supertrait closure*, so `dyn DoubleEnded`
  // carries `Iterator::next` in a slot of its own. compile_method_call indexes
  // the same numbering, and always for the trait the value was written as, so
  // the two cannot drift apart — see trait_flat_method_index.
  int method_count = trait_flat_method_count(trait);
  vt->method_count = method_count;
  vt->methods = al_alloc_zero(cg->al, sizeof(FunDef *) * (size_t)method_count);
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
  int mono_index = mono->vtable_count - 1; // stable: the list only grows

  // `Self` rides along harmlessly: a supertrait reference names only the
  // sub's own type parameters, never its receiver.
  Subst trait_args = cg_trait_ref_subst(cg, trait_ref, self);
  for (int i = 0; i < method_count; i++) {
    Type *owner_ref = NULL;
    TraitMethodDef *m = trait_flat_method(trait, i, &owner_ref);
    // a provided method the trait excluded from dispatch (object safety
    // partitioned per method — a combinator like `map`) gets no slot: the
    // checker already forbids reaching it through the `dyn`, so the NULL is
    // never read. Building one would mean instantiating a body whose own type
    // parameters the coercion site cannot supply.
    if (m->undispatchable) {
      continue;
    }
    // a super's slot is filled from the *super's* impl, so `owner_ref` is what
    // the lookup is keyed by. The closure states every entry — including the
    // trait's own — in the trait's declared parameters, so the reference the
    // `dyn` was written with is what turns `Into<T>` back into `Into<Celsius>`.
    owner_ref = subst_apply(&trait_args, owner_ref, cg->al);
    FunDef *target = cg_dyn_slot_target(cg, owner_ref, self, m->name, span);
    if (target == NULL) {
      char buf[64];
      type_sprintf(self, buf, sizeof(buf));
      diag_error(cg->diags, span,
                 "cannot build a trait object: '%s' has no body for '" SV_FMT
                 "::" SV_FMT "'",
                 buf, SV_ARG(owner_ref->as.trait.def->name), SV_ARG(m->name));
      cg->ok = false;
      return -1;
    }
    vt->methods[i] = target;
  }

  // the other half of the fixpoint: a pair recorded before this type was ever
  // coerced still needs its link from here.
  for (int i = 0; i < mono->upcast_count; i++) {
    if (mono->upcasts[i].from == trait_ref) {
      cg_link_upcast(cg, mono_index, i, span);
    }
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
  self = cg_subst(cg, self);
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

  Type *to_ref = cg_subst(cg, expr->coerce_dyn);

  if (self->kind == TY_DYN) {
    // an upcast: the value is already wrapped, so nothing is built here and
    // the concrete type is not needed — only which two spellings are in play.
    // Both must be pinned even so, since the pair is what every table's link
    // is keyed by: an abstract one would match no table and fail at run.
    if (type_is_abstract(to_ref)) {
      char buf[64];
      type_sprintf(to_ref, buf, sizeof(buf));
      diag_error(cg->diags, expr->span,
                 "cannot upcast to 'dyn %s': the trait's arguments are not "
                 "known here",
                 buf);
      cg->ok = false;
      return;
    }
    int pair = cg_upcast_pair(cg, self->as.dyn.trait, to_ref, expr->span);
    if (pair >= 0) {
      emit_slot(cg, OP_DYN_UPCAST, pair);
    }
    return;
  }

  int slot = cg_vtable_for(cg, to_ref, self, expr->span);
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
    Type *self = cg_subst(cg, mc->bound_self);

    // ...unless it is a trait object, where it stays abstract on purpose.
    // The receiver carries the table, so the slot is all codegen picks: the
    // position the trait fixes for this method name.
    if (self->kind == TY_DYN) {
      // over the closure, and over the closure of the trait the *value* names —
      // which is the same list cg_vtable_for laid the table out with, so the
      // index means the same slot on both sides even when the call arrived
      // through a supertrait's bound.
      TraitDef *trait = dyn_trait_def(self);
      int index = trait_flat_method_index(trait, mc->method_name);
      if (index < 0) {
        cg_error(cg, expr->span, "this method call");
        return;
      }
      TraitMethodDef *dyn_method = trait_flat_method(trait, index, NULL);

      // A provided method object safety left out of the vtable has no slot to
      // dispatch through — but it does not need one. It is a generic function
      // over `Self` (TraitMethodDef.default_impl), so instantiating it at
      // `Self = dyn Trait` compiles a body like any other, and the `self.m()`
      // calls inside it become vtable dispatches on their own. That is why a
      // trait object can satisfy the whole bound and not just the dispatchable
      // part of it: the vtable is what the *subset* governs.
      //
      // The cost is that this reaches the trait's default rather than an
      // override the concrete impl may have written for the same name, which
      // no vtable-less path can see. Rust makes the same trade for the same
      // reason (`dyn Iterator`'s `map` is `Iterator::map`).
      if (dyn_method->undispatchable) {
        fun = dyn_method->default_impl;
        if (fun == NULL) {
          // unreachable: a *required* undispatchable method makes the trait
          // non-object-safe, so no `dyn Trait` exists to have got here.
          cg_error(cg, expr->span, "this method call");
          return;
        }
      } else {
        // OP_DYN_METHOD leaves [fun, receiver], so the arguments push straight
        // on top of it and the ordinary OP_CALL below closes the call.
        compile_expr(cg, mc->object);
        emit2(cg, OP_DYN_METHOD, (uint8_t)index);
        cg_pushed(cg, 1); // one object in, [fun, receiver] out
        for (int i = 0; i < mc->arg_count; i++) {
          compile_expr(cg, mc->args[i]);
        }
        emit2(cg, OP_CALL, (uint8_t)(mc->arg_count + 1));
        return;
      }
    } else {
      // which trait the bound named, in this instantiation's terms: two impls
      // of one generic trait for one type differ only here.
      Type *trait_ref = cg_subst(cg, mc->bound_trait);

      fun = cg_bound_target(cg, self, trait_ref, mc->method_name, &impl_subst,
                            expr->span);
      if (fun == NULL) {
        cg_error(cg, expr->span, "this method call");
        return;
      }
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

  FunDef *target = cg_call_target(cg, fun, &mc->inst, &impl_subst, expr->span);
  if (target == NULL) {
    emit(cg, OP_UNIT); // reported; the program will not be run
    return;
  }
  const Instance *found = cg_inline_choice(cg, target);
  int base_depth = cg->depth;
  if (found == NULL) {
    cg_pushed(cg, 1);
    emit_slot(cg, OP_GET_GLOBAL, target->slot);
  }
  // the receiver is an argument like any other, at whatever position the
  // signature put `self` in
  Instance inst = found != NULL ? *found : (Instance){0};
  int k = 0;
  for (int i = 0; i < fun->param_count; i++) {
    if (fun->params[i].is_self) {
      compile_expr(cg, mc->object);
    } else {
      compile_expr(cg, mc->args[k++]);
    }
  }
  if (found != NULL) {
    cg_thread_arm(cg, expr);
    cg_inline_body(cg, &inst, base_depth, expr->span);
    return;
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
  emit(cg, OP_POP);   // is_ok was false; [instance]
  cg_emit_return(cg); // propagate Err(e) to the caller unchanged

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
  // remembered for `cg_last_variant`: this is the one construction a merge
  // below may decide was never needed.
  cg->last_enum = cg->chunk->count;
  cg->last_enum_def = enum_def;
  cg->last_enum_tag = variant->tag;
  emit_slot(cg, OP_ENUM, enum_def->slot);
  emit(cg, variant->tag);
}

// `d as? T` — the coercion recognised rather than performed. The whole test is
// that a vtable is memoised per (trait reference, concrete type), so asking
// `cg_vtable_for` for the table a coercion of `T` would have built yields the
// one pointer every `T` behind this trait carries and no other type does. The
// downcast site is therefore an ordinary vtable requester: a program that only
// ever asks *whether* something is a `Point` still slots that table, exactly as
// a coercion site would.
static void compile_downcast(Cg *cg, Expr *expr) {
  ExprCast *cast = &expr->as.cast;
  EnumDef *option = cast->option_enum;
  assert(option && cast->target && "downcast unresolved after checking");

  compile_expr(cg, cast->operand); // [dyn]

  Type *target = cg_subst(cg, cast->target);
  if (!type_is_concrete(target)) {
    char buf[64];
    type_sprintf(target, buf, sizeof(buf));
    diag_error(cg->diags, expr->span,
               "cannot downcast to '%s': the concrete type is not known here",
               buf);
    cg->ok = false;
    return;
  }
  int slot =
      cg_vtable_for(cg, cg_subst(cg, cast->dyn_trait), target, expr->span);
  if (slot < 0 || !cg_reach_enum(cg, option)) {
    emit(cg, OP_UNIT);
    return;
  }

  emit_slot(cg, OP_DYN_IS, slot);             // [dyn, is]
  int miss = emit_jump(cg, OP_JUMP_IF_FALSE); // peeks `is`
  emit(cg, OP_POP);                           // is (true) -> [dyn]
  emit(cg, OP_DYN_INNER);                     // [inner]
  emit_slot(cg, OP_ENUM, option->slot);       // [Some(inner)]
  emit(cg, cast->some_variant->tag);
  int done = emit_jump(cg, OP_JUMP);

  patch_jump(cg, miss);                 // [dyn, is]
  emit(cg, OP_POP);                     // is (false) -> [dyn]
  emit(cg, OP_POP);                     // the object was not it -> []
  emit_slot(cg, OP_ENUM, option->slot); // [None]
  emit(cg, cast->none_variant->tag);
  patch_jump(cg, done);
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
  emit_get_local(cg, acc->base_slot);
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
    cg_pushed(cg, 1);
    compile_expr(cg, pat->as.literal_expr);
    emit(cg, OP_EQ);
    cg_popped(cg, 1); // two operands in, one bool out
    jump_list_push(cg, fails, pat->span, emit_jump(cg, OP_JUMP_IF_FALSE));
    emit(cg, OP_POP);
    cg_popped(cg, 1); // only the matching side pops it; `fails` land above it
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
    cg_pushed(cg, 1); // the accessor's value, turned into its tag in place
    emit_const(cg, val_int(variant->tag));
    cg_pushed(cg, 1);
    emit(cg, OP_EQ);
    cg_popped(cg, 1);
    jump_list_push(cg, fails, pat->span, emit_jump(cg, OP_JUMP_IF_FALSE));
    emit(cg, OP_POP);
    cg_popped(cg, 1);

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
    cg_add_pushed_local(cg, pat->as.bind.name, pat->span);
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
        cg_add_pushed_local(cg, fp->ident.name, fp->span);
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
        cg_add_pushed_local(cg, fp->ident.name, fp->span);
      }
    }
    break;
  }
  }
}

// ── threading, the consumer's half ───────────────────────────────────────────
//
// A threaded exit jumps *past* every test the arms would have run, so an arm
// the tag alone cannot select would be skipped rather than tried: nothing below
// the variant may still ask a question, and a guard is a question.
static bool cg_pattern_irrefutable(Pattern *pat) {
  switch (pat->kind) {
  case PAT_WILDCARD:
  case PAT_BIND:
    return true;
  case PAT_LITERAL:
  case PAT_VARIANT:
    return false;
  case PAT_TUPLE:
    for (int i = 0; i < pat->as.tuple.count; i++) {
      if (!cg_pattern_irrefutable(pat->as.tuple.elems[i])) {
        return false;
      }
    }
    return true;
  case PAT_STRUCT:
    for (int i = 0; i < pat->as.struc.field_count; i++) {
      Pattern *sub = pat->as.struc.fields[i].sub_pattern;
      if (sub != NULL && !cg_pattern_irrefutable(sub)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

static bool cg_thread_pattern(Pattern *pat) {
  if (pat->kind != PAT_VARIANT) {
    return false;
  }
  VariantDef *variant = pat->as.variant.resolved_variant;
  // a field the pattern does not name would be read through an accessor the
  // one-field entry has already spent
  if (variant == NULL || pat->as.variant.field_count != variant->field_count) {
    return false;
  }
  for (int i = 0; i < pat->as.variant.field_count; i++) {
    Pattern *sub = pat->as.variant.fields[i].sub_pattern;
    if (sub != NULL && !cg_pattern_irrefutable(sub)) {
      return false;
    }
  }
  return true;
}

static bool cg_thread_one(CgThread *th, Pattern *pat) {
  if (!cg_thread_pattern(pat)) {
    return false;
  }
  th->takes[0] = pat->as.variant.resolved_variant;
  th->cont_count = 1;
  return true;
}

static bool cg_thread_arms(CgThread *th, MatchArm *arms, int count) {
  if (count == 0 || count > CG_MAX_THREAD_CONTS) {
    return false;
  }
  for (int i = 0; i < count; i++) {
    if (arms[i].guard != NULL || !cg_thread_pattern(arms[i].pattern)) {
      return false;
    }
    th->takes[i] = arms[i].pattern->as.variant.resolved_variant;
  }
  th->cont_count = count;
  return true;
}

// Compile the value the construct dispatches on, with its exits reporting
// themselves. The value it leaves *here*, if it leaves one, is one more exit —
// which is how a subject that is itself a construction (`match Some(x)`) needs
// no splice to be threaded.
static void cg_thread_subject(Cg *cg, CgThread *th, Expr *subject) {
  CgThread *saved = cg->thread;
  th->call = subject;
  cg->thread = th;
  compile_expr(cg, subject);
  cg->thread = saved;

  if (cg_unreachable(cg)) {
    return;
  }
  int cont = cg_thread_claim(cg, th);
  if (cont < 0) {
    return;
  }
  cg_thread_jump(th, cont, emit_jump(cg, OP_JUMP));
  cg->last_exit = cg->chunk->count;
}

// Whatever still arrives holding a built variant meets one tag test per
// continuation — the fail chain the arms used to be — and has its field taken
// out in place, so a path that reported its tag and a path that was asked enter
// the same code with the same stack. Emitted only where something arrives.
static void cg_thread_merge(Cg *cg, CgThread *th, int subject_slot) {
  if (cg_unreachable(cg)) {
    return;
  }
  int base = cg->depth; // the subject is the top of the stack, in its own slot
  for (int i = 0; i < th->cont_count; i++) {
    VariantDef *variant = th->takes[i];
    emit_get_local(cg, subject_slot);
    emit(cg, OP_TAG);
    cg_pushed(cg, 1);
    emit_const(cg, val_int(variant->tag));
    cg_pushed(cg, 1);
    emit(cg, OP_EQ);
    cg_popped(cg, 1);
    int miss = emit_jump(cg, OP_JUMP_IF_FALSE); // peeks; the arms pop it
    emit(cg, OP_POP);
    cg_popped(cg, 1);
    if (variant->field_count == 1) {
      emit2(cg, OP_FIELD_GET, 0); // in place: the slot is the top of the stack
    }
    cg_thread_jump(th, i, emit_jump(cg, OP_JUMP));
    patch_jump(cg, miss);
    cg->depth = base + 1;
    emit(cg, OP_POP);
    cg_popped(cg, 1);
  }
  cg_thread_jump(th, CG_THREAD_OTHER, emit_jump(cg, OP_JUMP));
  cg->last_exit = cg->chunk->count;
}

// Enter a continuation: everything that jumped here arrives with the same
// stack, whichever way it decided. False when nothing does.
static bool cg_thread_enter(Cg *cg, CgThread *th, int cont, int depth) {
  cg->depth = depth;
  if (th->jump_count[cont] == 0) {
    return false;
  }
  for (int i = 0; i < th->jump_count[cont]; i++) {
    patch_jump(cg, th->jumps[cont][i]);
  }
  return true;
}

// The names an entry binds. A one-field variant left its *field* in the
// subject's slot, so the sub-pattern reads that slot rather than a field of it
// — which is the whole of what deleting the construction costs the arm.
static void cg_thread_bind(Cg *cg, Pattern *pat, Accessor acc) {
  VariantDef *variant = pat->as.variant.resolved_variant;
  if (variant->field_count != 1) {
    compile_pattern_bind(cg, pat, acc);
    return;
  }
  FieldPat *fp = &pat->as.variant.fields[0];
  if (fp->sub_pattern != NULL) {
    compile_pattern_bind(cg, fp->sub_pattern, acc);
  } else {
    emit_accessor(cg, &acc);
    cg_add_pushed_local(cg, fp->ident.name, fp->span);
  }
}

// a `var` binding is a one-arm match with no guard and no body: the subject is
// already in `subject_slot`, so the whole statement is the two pattern passes.
// The checker rejects a refutable binding, but its answer is tri-state — an
// unsolved column type reports nothing — so the tests are still emitted and
// still trap through OP_MATCH_FAIL rather than binding from a value that never
// had the shape. For an irrefutable pattern `compile_pattern_test` emits
// nothing at all, so this costs the common case zero instructions.
//
// `else_block` is the whole of `var P = e else { .. }`: the failure path stops
// being the trap and becomes that block. The bindings have not been pushed
// yet, so `cg->local_count` there is exactly what the stack holds — the outer
// locals plus the subject — and a `break` or `continue` inside pops the right
// number on its way out. The trap stays *below* the block: the checker made it
// diverge, and if it ever fell through anyway the alternative would be reading
// names that were never bound.
static void compile_destructure(Cg *cg, Pattern *pat, int subject_slot,
                                Expr *else_block) {
  Accessor acc = {.base_slot = subject_slot};
  JumpList fails = {0};

  int fail_depth = cg->depth + 1; // a failed test's bool, above the subject
  compile_pattern_test(cg, pat, acc, &fails);
  int ok_depth = cg->depth;

  if (fails.count > 0) {
    int ok_jump = emit_jump(cg, OP_JUMP);
    jump_list_patch(cg, &fails);
    cg->depth = fail_depth;
    emit(cg, OP_POP); // the outstanding false from whichever test failed
    cg_popped(cg, 1);
    if (else_block != NULL) {
      compile_expr(cg, else_block);
      emit(cg, OP_POP); // unreached, and keeps the stack model honest
      cg_popped(cg, 1);
    }
    emit(cg, OP_MATCH_FAIL);
    patch_jump(cg, ok_jump);
    cg->depth = ok_depth;
  }

  compile_pattern_bind(cg, pat, acc);
}

// The same match with the tag tests gone: every arm is entered by a jump the
// construction below it already knew to make, and the merge above catches
// whatever path could not say. The subject's slot holds a one-field variant's
// *field*, which is what `cg_thread_bind` reads and what the final slide drops.
static void compile_match_threaded(Cg *cg, Expr *expr, CgThread *th,
                                   int subject_slot, int saved_locals_outer) {
  ExprMatch *match = &expr->as.match;
  int saved_locals = cg->local_count;
  int arm_depth = cg->depth;
  Accessor subject_acc = {.base_slot = subject_slot};
  JumpList end_jumps = {0};

  cg_thread_merge(cg, th, subject_slot);

  // a tag no arm names, which only the merge can produce: the trap the fall
  // past every arm used to be.
  if (cg_thread_enter(cg, th, CG_THREAD_OTHER, arm_depth)) {
    emit(cg, OP_MATCH_FAIL);
  }

  for (int i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    cg_thread_enter(cg, th, i, arm_depth);
    cg->local_count = saved_locals;
    cg_thread_bind(cg, arm->pattern, subject_acc);
    compile_expr(cg, arm->body);
    cg_close_scope(cg, saved_locals); // a closure may capture a bound name
    emit_slide(cg, cg->local_count - saved_locals);
    jump_list_push(cg, &end_jumps, arm->span, emit_jump(cg, OP_JUMP));
  }

  cg->local_count = saved_locals;
  jump_list_patch(cg, &end_jumps);
  emit_slide(cg, 1); // drop the hidden subject local, keep the arm result
  cg->local_count = saved_locals_outer;
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

  CgThread th = {0};
  if (cg_thread_arms(&th, match->arms, match->arm_count)) {
    cg_thread_subject(cg, &th, match->subject);
  } else {
    compile_expr(cg, match->subject);
  }
  int subject_slot = cg_add_local(cg, (StringView){0}, expr->span);
  int saved_locals = cg->local_count;

  if (th.taken > 0) {
    compile_match_threaded(cg, expr, &th, subject_slot, saved_locals_outer);
    return;
  }
  // every arm is an alternative reached from the same place, so each starts
  // from this depth rather than from whatever the arm before it left
  int arm_depth = cg->depth;
  Accessor subject_acc = {.base_slot = subject_slot};

  JumpList end_jumps = {0};

  for (int i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    cg->depth = arm_depth;
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
      int guard_forked = cg->depth; // the guard's bool is still live here
      emit(cg, OP_POP);
      cg_popped(cg, 1);

      compile_expr(cg, arm->body);
      cg_close_scope(cg, saved_locals); // a closure may capture a bound name
      emit_slide(cg, cg->local_count - saved_locals);
      jump_list_push(cg, &end_jumps, arm->span, emit_jump(cg, OP_JUMP));

      patch_jump(cg, guard_fail_jump);
      cg->depth = guard_forked;
      emit(cg, OP_POP);
      cg_popped(cg, 1);
      cg_close_scope(cg, saved_locals); // guard could have captured a bind too
      emit2(cg, OP_POPN, (uint8_t)(cg->local_count - saved_locals));
      cg_popped(cg, cg->local_count - saved_locals);
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

// `if var P = subject { .. } else { .. }` is compile_match with the arm list
// spelled out: one pattern whose failure lands in the `else` instead of in the
// next arm. The subject still lives in a hidden local — that is what the
// accessors read from — and is still slid out from under the result at the end.
static void compile_if_binding(Cg *cg, Expr *expr) {
  ExprIf *if_ = &expr->as.if_expr;
  int saved_outer = cg->local_count;

  CgThread th = {0};
  if (cg_thread_one(&th, if_->binding)) {
    cg_thread_subject(cg, &th, if_->condition);
  } else {
    compile_expr(cg, if_->condition);
  }
  int subject_slot = cg_add_local(cg, (StringView){0}, expr->span);
  int saved = cg->local_count;
  Accessor acc = {.base_slot = subject_slot};

  if (th.taken > 0) {
    // the `else` is every tag the binding does not name, so it is the merge's
    // fall-through as much as the failed test's
    int entry_depth = cg->depth;
    cg_thread_merge(cg, &th, subject_slot);

    cg_thread_enter(cg, &th, CG_THREAD_OTHER, entry_depth);
    if (if_->else_branch != NULL) {
      compile_expr(cg, if_->else_branch);
    } else {
      emit(cg, OP_UNIT);
      cg_pushed(cg, 1);
    }
    int else_end = emit_jump(cg, OP_JUMP);

    cg_thread_enter(cg, &th, 0, entry_depth);
    cg->local_count = saved;
    cg_thread_bind(cg, if_->binding, acc);
    compile_expr(cg, if_->then_block);
    cg_close_scope(cg, saved); // a closure may capture a bound name
    emit_slide(cg, cg->local_count - saved);

    patch_jump(cg, else_end);
    emit_slide(cg, 1); // drop the hidden subject, keep the branch's result
    cg->local_count = saved_outer;
    return;
  }
  // where a failed test lands: the subject is a local by now, and the test's
  // own bool is still above it because OP_JUMP_IF_FALSE never pops.
  int fail_depth = cg->depth + 1;

  JumpList fails = {0};
  compile_pattern_test(cg, if_->binding, acc, &fails);
  compile_pattern_bind(cg, if_->binding, acc);

  compile_expr(cg, if_->then_block);
  cg_close_scope(cg, saved); // a closure may capture a bound name
  emit_slide(cg, cg->local_count - saved);
  int end_jump = emit_jump(cg, OP_JUMP);
  cg->local_count = saved;

  jump_list_patch(cg, &fails);
  cg->depth = fail_depth;
  emit(cg, OP_POP); // the outstanding false from whichever test failed
  cg_popped(cg, 1);
  if (if_->else_branch != NULL) {
    compile_expr(cg, if_->else_branch);
  } else {
    emit(cg, OP_UNIT);
  }

  patch_jump(cg, end_jump);
  emit_slide(cg, 1); // drop the hidden subject, keep the branch's result
  cg->local_count = saved_outer;
}

// `while var P = subject { .. }`: the subject is re-evaluated every turn — that
// re-evaluation is what advances an iterator — so its hidden local is pushed
// and popped *inside* the loop, where a `for`'s loop-carried slots are
// allocated once above it. A failed test is the exit, so the loop needs no
// jump-out of its own.
static void compile_while_binding(Cg *cg, Expr *expr) {
  ExprWhile *wh = &expr->as.while_expr;
  int saved_outer = cg->local_count;

  CgLoop loop = {
      .label = expr->as.while_expr.label.name,
      .start = cg->chunk->count,
      .continue_is_backward = true,
      .continue_base = saved_outer,
      .break_base = saved_outer,
      .continue_depth = cg->depth,
      .break_depth = cg->depth,
      .parent = cg->loop,
  };
  cg->loop = &loop;

  CgThread th = {0};
  if (cg_thread_one(&th, wh->binding)) {
    cg_thread_subject(cg, &th, wh->condition);
  } else {
    compile_expr(cg, wh->condition);
  }
  int subject_slot = cg_add_local(cg, (StringView){0}, expr->span);
  int fail_depth = cg->depth + 1; // the failed test's bool, above the subject
  Accessor acc = {.base_slot = subject_slot};

  if (th.taken > 0) {
    int entry_depth = cg->depth;
    cg_thread_merge(cg, &th, subject_slot);

    cg_thread_enter(cg, &th, 0, entry_depth);
    cg_thread_bind(cg, wh->binding, acc);
    compile_expr(cg, wh->body);
    emit(cg, OP_POP); // the body's value
    cg_popped(cg, 1);
    cg_close_scope(cg, saved_outer);
    emit2(cg, OP_POPN, (uint8_t)(cg->local_count - saved_outer));
    cg_popped(cg, cg->local_count - saved_outer);
    cg->local_count = saved_outer;
    emit_loop(cg, loop.start);

    // any other tag ends the loop; the subject's slot holds a placeholder no
    // arm read
    cg_thread_enter(cg, &th, CG_THREAD_OTHER, entry_depth);
    emit(cg, OP_POP);
    cg_popped(cg, 1);
    for (int i = 0; i < loop.break_count; i++) {
      patch_jump(cg, loop.break_jumps[i]);
    }
    cg->loop = loop.parent;
    emit(cg, OP_UNIT); // loops evaluate to unit
    return;
  }

  JumpList fails = {0};
  compile_pattern_test(cg, wh->binding, acc, &fails);
  compile_pattern_bind(cg, wh->binding, acc);

  compile_expr(cg, wh->body);
  emit(cg, OP_POP); // the body's value
  cg_popped(cg, 1);
  cg_close_scope(cg, saved_outer); // detach captures before the slots go
  emit2(cg, OP_POPN, (uint8_t)(cg->local_count - saved_outer));
  cg_popped(cg, cg->local_count - saved_outer); // back to the loop's top
  cg->local_count = saved_outer;
  emit_loop(cg, loop.start);

  // the test failed: its bool is on top, the subject beneath it, and this turn
  // bound nothing.
  jump_list_patch(cg, &fails);
  cg->depth = fail_depth;
  emit(cg, OP_POP); // the test result
  cg_popped(cg, 1);
  emit(cg, OP_POP); // the subject
  cg_popped(cg, 1);
  for (int i = 0; i < loop.break_count; i++) { // breaks pop their own locals
    patch_jump(cg, loop.break_jumps[i]);
  }

  cg->loop = loop.parent;
  emit(cg, OP_UNIT); // loops evaluate to unit
}

static void compile_expr_inner(Cg *cg, Expr *expr);

// every expression goes through here, so a coercion recorded by the checker
// cannot be missed by whichever of the ~30 expression cases produced the
// value. The wrap is always the *last* thing: `expr` is compiled as the
// concrete type it is, then boxed.
//
// It is also the one place `Cg.depth` has to be right, and the reason no case
// below has to count its own pushes and pops: an expression leaves exactly one
// value, whatever it emitted to get there. So each nested `compile_expr` bumps
// the depth by one, an operand sequence accumulates without being asked to,
// and whatever a case did in between is overwritten here rather than carried.
static void compile_expr(Cg *cg, Expr *expr) {
  int base = cg->depth;
  compile_expr_inner(cg, expr);
  if (expr->coerce_dyn != NULL) {
    compile_coerce_dyn(cg, expr);
  }
  cg->depth = base + 1;
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
    // already decoded by the parser, which is the difference from a string:
    // a char is a value, so it needs no heap object and no intern.
    emit_const(cg, val_char(expr->as.char_val));
    break;

  case EXPR_INTERPOLATED: {
    ExprInterpolated *interp = &expr->as.interpolated;
    for (int i = 0; i < interp->seg_count; i++) {
      InterpolSeg *seg = &interp->segs[i];
      if (seg->kind == ISEG_TEXT) {
        emit_const(cg, val_obj(&cg_decode_string(cg, seg->text)->obj));
        cg_pushed(cg, 1); // a literal segment is not an expression node
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
    int slot = cg_exploded_field(cg, field->object, field->resolved_index);
    if (slot >= 0) {
      emit_get_local(cg, slot);
      break;
    }
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
      emit_get_local(cg, cg->self_slot);
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
        Type *self = cg_subst(cg, p->bound_self);
        Type *trait_ref = cg_subst(cg, p->bound_trait);
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

    int local = cg_find_local(cg, name);
    if (local >= 0) {
      emit_get_local(cg, cg->locals[local].slot);
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
  case EXPR_LOOP:
    compile_loop(cg, expr);
    break;
  case EXPR_FOR:
    compile_for(cg, expr);
    break;
  case EXPR_CALL:
    compile_call(cg, expr);
    break;

  case EXPR_UNARY: {
    compile_expr(cg, expr->as.unary.operand);
    switch (expr->as.unary.op) {
    case TOKEN_NOT:
      emit(cg, OP_NOT);
      break;
    case TOKEN_TILDE:
      emit(cg, OP_BIT_NOT);
      break;
    default:
      emit(cg, OP_NEG);
      break;
    }
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
    if (cast->fallible) {
      compile_downcast(cg, expr);
      break;
    }
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
           .self_slot = -1,
           .last_exit = -1,
           .last_get_local = -1,
           .last_enum = -1};
  cg.chunk = al_alloc_zero_for(mono->al, Chunk);
  chunk_init(cg.chunk, mono->al);

  for (int i = 0; i < fun->param_count; i++) {
    // the receiver is named, not anonymous: a closure in the body captures it
    // by that name like any other local (the parser leaves `self` nameless,
    // and `self` is a keyword, so nothing else can claim the slot).
    bool is_self = fun->params[i].is_self;
    StringView name = is_self ? sv_from_cstr("self") : fun->params[i].name;
    // the caller pushed these before the frame opened, so they are slots
    // 0..n-1 and the depth starts out counting them
    int slot = cg_add_pushed_local(&cg, name, body_of->span);
    if (is_self) {
      cg.self_slot = slot;
    }
  }

  cg.body_root = body_of->body;
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

  // A closure inside a body that is compiled more than once needs its own
  // FunDef per compilation, for the same reason `mono_request` copies the
  // enclosing one — `closure->def` is the AST node's, one for all of them, and
  // writing a chunk into it lets the last compilation win. (The `VAL_FUN`
  // constant the earlier chunks already hold names that shared def, so they
  // would go on to build closures over someone else's body.) Two things
  // compile a body more than once: a *generic* one, once per instantiation and
  // each a different program (`self.show()` in it resolves through
  // `cg->subst`); and an *inlined* one, once per splice.
  //
  // A non-generic body spliced nowhere is compiled once, so it keeps the AST's
  // def and pays nothing.
  if (cg->subst.count > 0 || cg->inline_pad != NULL) {
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
              .last_exit = -1,
              .last_get_local = -1,
              .last_enum = -1,
              .parent = cg};
  child.chunk = al_alloc_zero_for(cg->al, Chunk);
  chunk_init(child.chunk, cg->al);

  for (int i = 0; i < closure->param_count; i++) {
    // slots 0..n-1 of the closure's own frame, like any function's params
    cg_add_pushed_local(&child, closure->params[i].name, expr->span);
  }

  child.body_root = closure->body;
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
