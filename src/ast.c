#include "ast.h"
#include "allocator.h"
#include "string_utils.h"

#include <assert.h>
#include <stdio.h>

// the interning table starts here and doubles; it is open-addressed, so the
// capacity is always a power of two and the load is kept under 70%.
#define TYPE_INTERN_INIT_CAP 256

typedef struct {
  Type **entries; // NULL until the first intern
  int count;
  int cap;
} TypeInternTable;

// Interning is process-global rather than per-compiler because type identity
// is pointer equality: two `Type *` compare equal only if the same table
// produced them. The entries (and, now that the table has a growth path, the
// array itself) live in the compiler's arena, so `type_intern_reset` has to
// run before that arena is torn down — see compiler_destroy.
static TypeInternTable g_intern;

// The three operations a TY_GENERIC's associated-type bound list needs from
// interning — hash, compare, canonicalise — each written once and recursing
// into the nested list milestone 76 added. The recursion terminates because
// only a trait ref's bindings can build a nested entry, and only a where-lhs
// (capped at one associated type) can build a level for them to sit on.
static void assoc_bounds_hash(uint32_t *h, const AssocBound *abs, int count) {
  for (int i = 0; i < count; i++) {
    const AssocBound *ab = &abs[i];
    for (int j = 0; j < (int)ab->name.len; j++)
      *h = *h * 31 + (uint8_t)ab->name.chars[j];
    for (int j = 0; j < ab->bound_count; j++)
      *h = *h * 31 + (uint32_t)(uintptr_t)ab->bounds[j];
    *h = *h * 31 + (uint32_t)(uintptr_t)ab->equals;
    *h = *h * 31 + (uint32_t)(uintptr_t)ab->owner;
    assoc_bounds_hash(h, ab->assoc_bounds, ab->assoc_bound_count);
  }
}

static bool assoc_bounds_equal(const AssocBound *x, const AssocBound *y,
                               int count) {
  for (int i = 0; i < count; i++) {
    if (!sv_equal(x[i].name, y[i].name) || x[i].owner != y[i].owner ||
        x[i].bound_count != y[i].bound_count || x[i].equals != y[i].equals ||
        x[i].assoc_bound_count != y[i].assoc_bound_count) {
      return false;
    }
    for (int j = 0; j < x[i].bound_count; j++) {
      if (x[i].bounds[j] != y[i].bounds[j]) {
        return false;
      }
    }
    if (!assoc_bounds_equal(x[i].assoc_bounds, y[i].assoc_bounds,
                            x[i].assoc_bound_count)) {
      return false;
    }
  }
  return true;
}

static uint32_t type_hash(const Type *t) {
  uint32_t h = (uint32_t)t->kind * 2654435761u;
  switch (t->kind) {
  case TY_GENERIC:
    for (int i = 0; i < (int)t->as.generic.name.len; i++)
      h = h * 31 + (uint8_t)t->as.generic.name.chars[i];
    for (int i = 0; i < t->as.generic.bound_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.generic.bounds[i];
    assoc_bounds_hash(&h, t->as.generic.assoc_bounds,
                      t->as.generic.assoc_bound_count);
    break;
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.tuple.elem_types[i];
    break;
  case TY_ARRAY:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.array.elem_type;
    break;
  case TY_FUNCTION:
    for (int i = 0; i < t->as.fun.param_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.fun.param_types[i];
    h = h * 31 + (uint32_t)(uintptr_t)t->as.fun.return_type;
    break;
  case TY_STRUCT:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.struc.def;
    for (int i = 0; i < t->as.struc.type_arg_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.struc.type_args[i];
    break;
  case TY_ENUM:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.enm.def;
    for (int i = 0; i < t->as.enm.type_arg_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.enm.type_args[i];
    break;
  case TY_TRAIT:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.trait.def;
    for (int i = 0; i < t->as.trait.type_arg_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.trait.type_args[i];
    break;
  case TY_DYN:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.dyn.trait;
    for (int i = 0; i < t->as.dyn.assoc_type_count; i++)
      h = h * 31 + (uint32_t)(uintptr_t)t->as.dyn.assoc_types[i];
    break;
  case TY_ASSOC:
    h = h * 31 + (uint32_t)(uintptr_t)t->as.assoc.base;
    h = h * 31 + (uint32_t)(uintptr_t)t->as.assoc.trait;
    for (int i = 0; i < (int)t->as.assoc.assoc_name.len; i++)
      h = h * 31 + (uint8_t)t->as.assoc.assoc_name.chars[i];
    break;
  case TY_UNKNOWN:
    h = h * 31 + t->as.unknown.id;
    h = h * 31 + (uint32_t)(uintptr_t)t->as.unknown.bound;
    break;
  default:
    break; // singletons: kind alone is enough
  }
  return h;
}

static bool type_structurally_equal(const Type *a, const Type *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case TY_GENERIC:
    if (!sv_equal(a->as.generic.name, b->as.generic.name)) {
      return false;
    }
    if (a->as.generic.bound_count != b->as.generic.bound_count) {
      return false;
    }
    for (int i = 0; i < a->as.generic.bound_count; i++) {
      if (a->as.generic.bounds[i] != b->as.generic.bounds[i]) {
        return false;
      }
    }
    // an associated-type bound is part of what the parameter *is*: `I` and
    // `I where I.Item: Ord` accept different instantiations, so they cannot
    // share an interned type. Both lists are canonically ordered by ty_generic.
    if (a->as.generic.assoc_bound_count != b->as.generic.assoc_bound_count) {
      return false;
    }
    return assoc_bounds_equal(a->as.generic.assoc_bounds,
                              b->as.generic.assoc_bounds,
                              a->as.generic.assoc_bound_count);
  case TY_TUPLE:
    if (a->as.tuple.elem_count != b->as.tuple.elem_count)
      return false;
    for (int i = 0; i < a->as.tuple.elem_count; i++)
      if (a->as.tuple.elem_types[i] != b->as.tuple.elem_types[i])
        return false;
    return true;
  case TY_ARRAY:
    return a->as.array.elem_type == b->as.array.elem_type;
  case TY_FUNCTION:
    if (a->as.fun.param_count != b->as.fun.param_count)
      return false;
    for (int i = 0; i < a->as.fun.param_count; i++)
      if (a->as.fun.param_types[i] != b->as.fun.param_types[i])
        return false;
    return a->as.fun.return_type == b->as.fun.return_type;
  case TY_STRUCT:
    if (a->as.struc.def != b->as.struc.def)
      return false;
    if (a->as.struc.type_arg_count != b->as.struc.type_arg_count)
      return false;
    for (int i = 0; i < a->as.struc.type_arg_count; i++)
      if (a->as.struc.type_args[i] != b->as.struc.type_args[i])
        return false;
    return true;
  case TY_ENUM:
    if (a->as.enm.def != b->as.enm.def)
      return false;
    if (a->as.enm.type_arg_count != b->as.enm.type_arg_count)
      return false;
    for (int i = 0; i < a->as.enm.type_arg_count; i++)
      if (a->as.enm.type_args[i] != b->as.enm.type_args[i])
        return false;
    return true;
  case TY_TRAIT:
    if (a->as.trait.def != b->as.trait.def)
      return false;
    if (a->as.trait.type_arg_count != b->as.trait.type_arg_count)
      return false;
    for (int i = 0; i < a->as.trait.type_arg_count; i++)
      if (a->as.trait.type_args[i] != b->as.trait.type_args[i])
        return false;
    return true;
  case TY_DYN:
    if (a->as.dyn.trait != b->as.dyn.trait)
      return false;
    if (a->as.dyn.assoc_type_count != b->as.dyn.assoc_type_count)
      return false;
    for (int i = 0; i < a->as.dyn.assoc_type_count; i++)
      if (a->as.dyn.assoc_types[i] != b->as.dyn.assoc_types[i])
        return false;
    return true;
  case TY_ASSOC:
    return a->as.assoc.base == b->as.assoc.base &&
           a->as.assoc.trait == b->as.assoc.trait &&
           sv_equal(a->as.assoc.assoc_name, b->as.assoc.assoc_name);
  default:
    return true; // singletons equal by kind
  }
}

// lexicographic order over two names, for canonicalising a set keyed by one.
// Not in string_utils because nothing else needs an *ordering* on a name —
// every other comparison in the compiler is equality.
static int sv_order(StringView a, StringView b) {
  int n = a.len < b.len ? a.len : b.len;
  int c = n > 0 ? memcmp(a.chars, b.chars, (size_t)n) : 0;
  return c != 0 ? c : a.len - b.len;
}

static void sort_bounds(Type **bounds, int count) {
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (bounds[j] > bounds[j + 1]) {
        Type *temp = bounds[j];
        bounds[j] = bounds[j + 1];
        bounds[j + 1] = temp;
      }
    }
  }
}

// Copy an associated-type bound list into canonical order: entries by name,
// each entry's trait list by pointer like any bound list, and the same again
// for the nested list. The caller's array is borrowed (often a stack buffer),
// so this always copies.
static AssocBound *assoc_bounds_canon(const AssocBound *src, int count,
                                      Allocator *al) {
  if (count == 0) {
    return NULL;
  }
  AssocBound *out = al_alloc(al, (size_t)count * sizeof(AssocBound));
  for (int i = 0; i < count; i++) {
    out[i] = src[i];
    Type **b = al_alloc(al, (size_t)out[i].bound_count * sizeof(Type *));
    for (int j = 0; j < out[i].bound_count; j++) {
      b[j] = src[i].bounds[j];
    }
    sort_bounds(b, out[i].bound_count);
    out[i].bounds = b;
    out[i].assoc_bounds =
        assoc_bounds_canon(src[i].assoc_bounds, src[i].assoc_bound_count, al);
  }
  // Ties on the name are broken by the owning trait, because since milestone 76
  // a name no longer identifies an entry: `Add<Output = T> + Mul<Output = int>`
  // is two entries called `Output`, and without the tie-break the two orders of
  // writing that bound would sort into two different lists and intern as two
  // different types.
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      int c = sv_order(out[j].name, out[j + 1].name);
      if (c > 0 || (c == 0 && out[j].owner > out[j + 1].owner)) {
        AssocBound tmp = out[j];
        out[j] = out[j + 1];
        out[j + 1] = tmp;
      }
    }
  }
  return out;
}

static inline bool type_is_internable(Type *t) {
  if (t->kind == TY_UNKNOWN || t->kind == TY_POISON) {
    return false;
  }
  switch (t->kind) {
    // for container
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (!type_is_internable(t->as.tuple.elem_types[i])) {
        return false;
      }
    }
    break;
  case TY_ARRAY:
    if (!type_is_internable(t->as.array.elem_type)) {
      return false;
    }
    break;
  case TY_ASSOC:
    if (!type_is_internable(t->as.assoc.base)) {
      return false;
    }
    break;
  case TY_STRUCT:
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (!type_is_internable(t->as.struc.type_args[i])) {
        return false;
      }
    }
    break;
  case TY_ENUM:
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      if (!type_is_internable(t->as.enm.type_args[i])) {
        return false;
      }
    }
    break;
  case TY_TRAIT:
    for (int i = 0; i < t->as.trait.type_arg_count; i++) {
      if (!type_is_internable(t->as.trait.type_args[i])) {
        return false;
      }
    }
    break;
  case TY_DYN:
    // a binding — or a trait argument — can still be an unsolved unknown
    // (`[dyn Iterator<Item = T>]` at a call site), and a container of a
    // non-internable type is not internable — the same rule every case above
    // follows.
    if (!type_is_internable(t->as.dyn.trait)) {
      return false;
    }
    for (int i = 0; i < t->as.dyn.assoc_type_count; i++) {
      if (!type_is_internable(t->as.dyn.assoc_types[i])) {
        return false;
      }
    }
    break;
  default:
    break;
  }
  return true;
}

// place `t` in `entries` without checking for a duplicate: only ever used to
// refill a freshly grown table, whose contents are already distinct.
static void type_intern_put(Type **entries, uint32_t mask, Type *t) {
  uint32_t slot = type_hash(t) & mask;
  while (entries[slot]) {
    slot = (slot + 1) & mask;
  }
  entries[slot] = t;
}

// grow (and rehash) if inserting one more would push the load past 70%. This
// has to run *before* a probe rather than at the point of insertion: a slot
// index is only meaningful under the mask it was computed with, so resizing
// between finding a free slot and filling it would file the entry where no
// later lookup looks — the same hazard as sorting a generic's bounds after
// hashing it.
static void type_intern_reserve(Allocator *al) {
  if ((g_intern.count + 1) * 10 < g_intern.cap * 7) {
    return;
  }
  int new_cap = g_intern.cap == 0 ? TYPE_INTERN_INIT_CAP : g_intern.cap * 2;
  Type **entries = al_alloc_zero(al, sizeof(Type *) * (size_t)new_cap);
  for (int i = 0; i < g_intern.cap; i++) {
    if (g_intern.entries[i]) {
      type_intern_put(entries, (uint32_t)new_cap - 1, g_intern.entries[i]);
    }
  }
  g_intern.entries = entries;
  g_intern.cap = new_cap;
}

static Type *type_intern(Type *t, Allocator *al) {
  if (!type_is_internable(t)) {
    return t;
  }
  type_intern_reserve(al);

  uint32_t mask = (uint32_t)g_intern.cap - 1;
  uint32_t slot = type_hash(t) & mask;
  while (true) {
    Type *entry = g_intern.entries[slot];
    if (!entry) {
      // NB: bound order must already be canonical here — sorting after
      // type_hash ran would file the entry under a slot no later lookup
      // computes. ty_generic sorts before probing, so there's nothing to do.

      g_intern.entries[slot] = t;
      g_intern.count++;
      return t;
    }
    if (type_structurally_equal(entry, t))
      return entry;
    slot = (slot + 1) & mask;
  }
}

static Type *type_intern_lookup(Type *probe) {
  if (g_intern.cap == 0) {
    return NULL;
  }
  uint32_t mask = (uint32_t)g_intern.cap - 1;
  uint32_t slot = type_hash(probe) & mask;
  while (g_intern.entries[slot]) {
    if (type_structurally_equal(g_intern.entries[slot], probe)) {
      return g_intern.entries[slot];
    }
    slot = (slot + 1) & mask;
  }
  return NULL; // miss
}

// the table and everything in it is arena memory, so the global cannot be left
// pointing at it once that arena is gone. Unreachable from `main` today (one
// compile per process), but it is a use-after-free waiting for the second.
void type_intern_reset(void) { g_intern = (TypeInternTable){0}; }

Type *ty_int(void) {
  static Type int_type = {.kind = TY_INT};
  return &int_type;
}

Type *ty_float(void) {
  static Type float_type = {.kind = TY_FLOAT};
  return &float_type;
}

Type *ty_bool(void) {
  static Type bool_type = {.kind = TY_BOOL};
  return &bool_type;
}

Type *ty_char(void) {
  static Type char_type = {.kind = TY_CHAR};
  return &char_type;
}

Type *ty_string(void) {
  static Type string_type = {.kind = TY_STRING};
  return &string_type;
}

Type *ty_strbuf(void) {
  static Type strbuf_type = {.kind = TY_STRBUF};
  return &strbuf_type;
}

Type *ty_unit(void) {
  static Type unit_type = {.kind = TY_UNIT};
  return &unit_type;
}

Type *ty_never(void) {
  static Type never = {.kind = TY_NEVER};
  return &never;
}

Type *ty_range(void) {
  static Type range = {.kind = TY_RANGE};
  return &range;
}

Type *ty_poison(void) {
  static Type poison = {.kind = TY_POISON};
  return &poison;
}

Type *ty_unknown(uint32_t id, Type *bound, Allocator *al) {
  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_UNKNOWN;
  t->as.unknown.id = id;
  t->as.unknown.bound = bound;
  return t;
}

Type *ty_fun(Type **params, int param_count, Type *ret, Allocator *al) {
  Type probe = {.kind = TY_FUNCTION,
                .as.fun = {
                    .param_types = params,
                    .param_count = param_count,
                    .return_type = ret,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_for(al, Type);
  t->kind = TY_FUNCTION;
  t->as.fun.param_types = al_alloc(al, param_count * sizeof(Type *));
  for (int i = 0; i < param_count; i++) {
    t->as.fun.param_types[i] = params[i];
  }
  t->as.fun.param_count = param_count;
  t->as.fun.return_type = ret;
  return type_intern(t, al);
}

Type *ty_tuple(Type **elems, int elem_count, Allocator *al) {
  Type probe = {.kind = TY_TUPLE,
                .as.tuple = {
                    .elem_types = elems,
                    .elem_count = elem_count,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }
  Type *t = al_alloc_for(al, Type);
  t->kind = TY_TUPLE;
  t->as.tuple.elem_types = al_alloc(al, elem_count * sizeof(Type *));
  for (int i = 0; i < elem_count; i++) {
    t->as.tuple.elem_types[i] = elems[i];
  }
  t->as.tuple.elem_count = elem_count;
  return type_intern(t, al);
}

Type *ty_array(Type *elem, Allocator *al) {
  Type probe = {.kind = TY_ARRAY,
                .as.array = {
                    .elem_type = elem,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }
  Type *t = al_alloc_for(al, Type);
  t->kind = TY_ARRAY;
  t->as.array.elem_type = elem;
  return type_intern(t, al);
}

Type *ty_generic(StringView name, Type **bounds, int bound_count,
                 AssocBound *assoc_bounds, int assoc_bound_count,
                 Allocator *al) {
  // Canonicalise bound order up front: `T: A + B` and `T: B + A` denote the
  // same type, and both type_hash and the probe below are order-sensitive.
  // (The caller's array is borrowed, so sort a copy.)
  Type **sorted = NULL;
  if (bound_count > 0) {
    sorted = al_alloc(al, bound_count * sizeof(Type *));
    for (int i = 0; i < bound_count; i++) {
      sorted[i] = bounds[i];
    }
    sort_bounds(sorted, bound_count);
  }

  // Same argument one level down: the associated-type bounds are a set too, so
  // `where T.A: X, T.B: Y` and `where T.B: Y, T.A: X` must intern as one type.
  AssocBound *assoc_sorted =
      assoc_bounds_canon(assoc_bounds, assoc_bound_count, al);

  Type probe = {.kind = TY_GENERIC,
                .as.generic = {
                    .name = name,
                    .bounds = sorted,
                    .bound_count = bound_count,
                    .assoc_bounds = assoc_sorted,
                    .assoc_bound_count = assoc_bound_count,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_GENERIC;
  t->as.generic.name = name;
  t->as.generic.bounds = sorted;
  t->as.generic.bound_count = bound_count;
  t->as.generic.assoc_bounds = assoc_sorted;
  t->as.generic.assoc_bound_count = assoc_bound_count;
  return type_intern(t, al);
}

Type *ty_assoc(Type *base, StringView assoc_name, TraitDef *trait,
               Allocator *al) {
  // An equality binding is discharged *here*, by never building the projection
  // at all: if the parameter's declaration says `J: Iterator<Item = I.Item>`,
  // then `J.Item` **is** `I.Item`, and returning it makes the two the same
  // interned pointer. Every downstream reader — unification, substitution,
  // infer_apply, codegen — then needs nothing, because there is no second
  // spelling left for them to reconcile.
  //
  // One hop suffices and cannot cycle: the type stored in `equals` was itself
  // built through this constructor, so it is already collapsed, and a bound may
  // name only a type parameter declared earlier in the same list — or, since
  // milestone 75, the subject itself.
  if (base->kind == TY_GENERIC) {
    for (int i = 0; i < base->as.generic.assoc_bound_count; i++) {
      const AssocBound *ab = &base->as.generic.assoc_bounds[i];
      // the trait is part of the key: one parameter may carry an `Output` from
      // each of two operator traits, and they are different projections.
      if (ab->equals == NULL || !sv_equal(ab->name, assoc_name) ||
          (ab->owner != NULL && trait != NULL && ab->owner != trait)) {
        continue;
      }
      // `T: Iter<Out = T>` binds the projection to the parameter itself, and
      // what sits in `equals` is the bound-less placeholder the parameter was
      // resolved against (see resolve_type_params). Same name is same
      // parameter, so hand back the spelling the caller already has — which is
      // the bounded one, and the only one its body will compare against.
      if (ab->equals->kind == TY_GENERIC &&
          sv_equal(ab->equals->as.generic.name, base->as.generic.name)) {
        return base;
      }
      return ab->equals;
    }
  }

  // The same discharge one level down, for a binding whose subject is itself a
  // projection: `where Self.Item: Add<Output = Self.Item>` makes
  // `Self.Item.Output` **be** `Self.Item`, which is the only way a bounded
  // generic can say what an operator over its element type returns.
  //
  // The self-reference is compared by *name* rather than by pointer, for the
  // reason milestone 75 found one level up: the type in `equals` was built
  // against the parameter's bound-less placeholder, so the base it names is a
  // second interned node spelling the same parameter. Two names decide it here
  // — the parameter's and the projection's — because that is the whole path.
  if (base->kind == TY_ASSOC && base->as.assoc.base->kind == TY_GENERIC) {
    const TypeGeneric *g = &base->as.assoc.base->as.generic;
    for (int i = 0; i < g->assoc_bound_count; i++) {
      const AssocBound *outer = &g->assoc_bounds[i];
      if (!sv_equal(outer->name, base->as.assoc.assoc_name) ||
          (outer->owner != NULL && base->as.assoc.trait != NULL &&
           outer->owner != base->as.assoc.trait)) {
        continue;
      }
      for (int j = 0; j < outer->assoc_bound_count; j++) {
        const AssocBound *ab = &outer->assoc_bounds[j];
        if (ab->equals == NULL || !sv_equal(ab->name, assoc_name) ||
            (ab->owner != NULL && trait != NULL && ab->owner != trait)) {
          continue;
        }
        if (ab->equals->kind == TY_ASSOC &&
            ab->equals->as.assoc.base->kind == TY_GENERIC &&
            sv_equal(ab->equals->as.assoc.base->as.generic.name, g->name) &&
            sv_equal(ab->equals->as.assoc.assoc_name,
                     base->as.assoc.assoc_name)) {
          return base;
        }
        return ab->equals;
      }
      break;
    }
  }

  Type probe = {.kind = TY_ASSOC,
                .as.assoc = {
                    .base = base,
                    .trait = trait,
                    .assoc_name = assoc_name,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_ASSOC;
  t->as.assoc.base = base;
  t->as.assoc.assoc_name = assoc_name;
  t->as.assoc.trait = trait;
  return type_intern(t, al);
}

Type *ty_struct(StructDef *def, Type **args, int argc, Allocator *al) {
  Type probe = {.kind = TY_STRUCT,
                .as.struc = {
                    .def = def,
                    .type_args = args,
                    .type_arg_count = argc,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_STRUCT;
  t->as.struc.def = def;
  t->as.struc.type_args = al_alloc(al, argc * sizeof(Type *));
  for (int i = 0; i < argc; i++) {
    t->as.struc.type_args[i] = args[i];
  }
  t->as.struc.type_arg_count = argc;
  return type_intern(t, al);
}

Type *ty_enum(EnumDef *def, Type **args, int argc, Allocator *al) {
  Type probe = {.kind = TY_ENUM,
                .as.enm = {
                    .def = def,
                    .type_args = args,
                    .type_arg_count = argc,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_ENUM;
  t->as.enm.def = def;
  t->as.enm.type_args = al_alloc(al, argc * sizeof(Type *));
  for (int i = 0; i < argc; i++) {
    t->as.enm.type_args[i] = args[i];
  }
  t->as.enm.type_arg_count = argc;
  return type_intern(t, al);
}

Type *ty_trait(TraitDef *def, Type **args, int argc, Allocator *al) {
  Type probe = {.kind = TY_TRAIT,
                .as.trait = {
                    .def = def,
                    .type_args = args,
                    .type_arg_count = argc,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_TRAIT;
  t->as.trait.def = def;
  t->as.trait.type_args = al_alloc(al, argc * sizeof(Type *));
  for (int i = 0; i < argc; i++) {
    t->as.trait.type_args[i] = args[i];
  }
  t->as.trait.type_arg_count = argc;
  return type_intern(t, al);
}

Type *ty_dyn(Type *trait, Type **assoc_types, int assoc_type_count,
             Allocator *al) {
  Type probe = {.kind = TY_DYN,
                .as.dyn = {
                    .trait = trait,
                    .assoc_types = assoc_types,
                    .assoc_type_count = assoc_type_count,
                }};
  Type *interned = type_intern_lookup(&probe);
  if (interned) {
    return interned;
  }

  Type *t = al_alloc_zero_for(al, Type);
  t->kind = TY_DYN;
  t->as.dyn.trait = trait;
  t->as.dyn.assoc_types = al_alloc(al, assoc_type_count * sizeof(Type *));
  for (int i = 0; i < assoc_type_count; i++) {
    t->as.dyn.assoc_types[i] = assoc_types[i];
  }
  t->as.dyn.assoc_type_count = assoc_type_count;
  return type_intern(t, al);
}

bool type_is_abstract(const Type *t) {
  switch (t->kind) {
  case TY_GENERIC:
  case TY_UNKNOWN:
  case TY_ASSOC:
    return true;
  case TY_FUNCTION:
    for (int i = 0; i < t->as.fun.param_count; i++) {
      if (type_is_abstract(t->as.fun.param_types[i])) {
        return true;
      }
    }
    return type_is_abstract(t->as.fun.return_type);
  case TY_TUPLE:
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (type_is_abstract(t->as.tuple.elem_types[i])) {
        return true;
      }
    }
    return false;
  case TY_STRUCT:
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (type_is_abstract(t->as.struc.type_args[i])) {
        return true;
      }
    }
    return false;
  case TY_ENUM:
    for (int i = 0; i < t->as.enm.type_arg_count; i++) {
      if (type_is_abstract(t->as.enm.type_args[i])) {
        return true;
      }
    }
    return false;
  case TY_ARRAY:
    return type_is_abstract(t->as.array.elem_type);
  case TY_TRAIT:
    // the trait itself is a name; what can be abstract is what it was applied
    // to.
    for (int i = 0; i < t->as.trait.type_arg_count; i++) {
      if (type_is_abstract(t->as.trait.type_args[i])) {
        return true;
      }
    }
    return false;
  case TY_DYN:
    if (type_is_abstract(t->as.dyn.trait)) {
      return true;
    }
    for (int i = 0; i < t->as.dyn.assoc_type_count; i++) {
      if (type_is_abstract(t->as.dyn.assoc_types[i])) {
        return true;
      }
    }
    return false;
  default:
    return false;
  }
}

// ── reading a trait through its supertrait closure ───────────────────────────
//
// Four questions used to be answered by walking a trait's own `methods` and
// `assoc_types`; with supertraits each walks the closure instead, and these are
// the accessors that flatten it. The order — supers first, in closure order,
// each trait's items in declaration order — is fixed here and *is* the `dyn`
// vtable layout, so build and dispatch cannot disagree.
//
// There is no diamond problem to solve, which is why a flat numbering is
// enough: a trait object always carries the table of the trait it was written
// as, and every dispatch on it indexes that same trait's closure. Two tables
// never have to agree — the upcast (milestone 78) does not make them, it swaps
// one for the other, precisely because a super's numbering is its own.

int trait_flat_method_count(const TraitDef *trait) {
  int n = 0;
  for (int i = 0; i < trait->flat_count; i++) {
    n += trait->flat[i].def->method_count;
  }
  return n;
}

TraitMethodDef *trait_flat_method(const TraitDef *trait, int index,
                                  Type **owner_ref) {
  for (int i = 0; i < trait->flat_count; i++) {
    TraitDef *owner = trait->flat[i].def;
    if (index < owner->method_count) {
      if (owner_ref != NULL) {
        *owner_ref = trait->flat[i].ref;
      }
      return &owner->methods[index];
    }
    index -= owner->method_count;
  }
  return NULL;
}

int trait_flat_method_index(const TraitDef *trait, StringView name) {
  int base = 0;
  for (int i = 0; i < trait->flat_count; i++) {
    TraitDef *owner = trait->flat[i].def;
    for (int j = 0; j < owner->method_count; j++) {
      if (sv_equal(owner->methods[j].name, name)) {
        return base + j;
      }
    }
    base += owner->method_count;
  }
  return -1;
}

int trait_flat_assoc_count(const TraitDef *trait) {
  int n = 0;
  for (int i = 0; i < trait->flat_count; i++) {
    n += trait->flat[i].def->assoc_type_count;
  }
  return n;
}

StringView trait_flat_assoc_name(const TraitDef *trait, int index,
                                 TraitDef **owner_def) {
  for (int i = 0; i < trait->flat_count; i++) {
    TraitDef *owner = trait->flat[i].def;
    if (index < owner->assoc_type_count) {
      if (owner_def != NULL) {
        *owner_def = owner;
      }
      return owner->assoc_types[index].name;
    }
    index -= owner->assoc_type_count;
  }
  if (owner_def != NULL) {
    *owner_def = NULL;
  }
  return (StringView){0};
}

int trait_flat_assoc_index(const TraitDef *trait, StringView name) {
  int base = 0;
  for (int i = 0; i < trait->flat_count; i++) {
    TraitDef *owner = trait->flat[i].def;
    for (int j = 0; j < owner->assoc_type_count; j++) {
      if (sv_equal(owner->assoc_types[j].name, name)) {
        return base + j;
      }
    }
    base += owner->assoc_type_count;
  }
  return -1;
}

Type *ty_dyn_assoc(const Type *dyn, StringView name) {
  // the table is laid out over the trait's *supertrait closure* (TYNODE_ASSOC),
  // so a super's associated type is reachable through the sub's trait object.
  int slot = trait_flat_assoc_index(dyn_trait_def(dyn), name);
  if (slot < 0 || slot >= dyn->as.dyn.assoc_type_count) {
    return NULL;
  }
  return dyn->as.dyn.assoc_types[slot];
}

// bool types_equal(const Type *a, const Type *b) { return a == b; }

bool type_is_numeric(const Type *t) {
  return t->kind == TY_INT || t->kind == TY_FLOAT;
}

const char *type_name(const Type *t) {
  static char buf[64];
  type_name_sprintf(t, buf, sizeof(buf));
  return buf;
}

int type_name_sprintf(const Type *t, char *buf, size_t buf_size) {
  if (!t) {
    return snprintf(buf, buf_size, "NULL_TYPE");
  }
  switch (t->kind) {
  case TY_INT:
    return snprintf(buf, buf_size, "int");
  case TY_FLOAT:
    return snprintf(buf, buf_size, "float");
  case TY_BOOL:
    return snprintf(buf, buf_size, "bool");
  case TY_CHAR:
    return snprintf(buf, buf_size, "char");
  case TY_STRING:
    return snprintf(buf, buf_size, "String");
  case TY_STRBUF:
    return snprintf(buf, buf_size, "StringBuf");
  case TY_UNIT:
    return snprintf(buf, buf_size, "()");
  case TY_NEVER:
    return snprintf(buf, buf_size, "!");
  case TY_UNKNOWN:
    if (t->as.unknown.bound) {
      return type_name_sprintf(t->as.unknown.bound, buf, buf_size);
    } else {
      return snprintf(buf, buf_size, "_");
    }
  case TY_POISON:
    return snprintf(buf, buf_size, "<POISON>");
  case TY_GENERIC:
    return snprintf(buf, buf_size, SV_FMT,
                    SV_ARG(t->as.generic.name)); // todo: include bounds
  case TY_FUNCTION:
    return snprintf(buf, buf_size, "fun(...)");
  case TY_TUPLE:
    return snprintf(buf, buf_size, "(...)");
  case TY_STRUCT:
    return snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.struc.def->name));
  case TY_ENUM:
    return snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.enm.def->name));
  case TY_TRAIT:
    return snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.trait.def->name));
  case TY_DYN:
    return snprintf(buf, buf_size, "dyn " SV_FMT,
                    SV_ARG(dyn_trait_def(t)->name));
  case TY_ARRAY:
    return snprintf(buf, buf_size, "Array<...>");
  case TY_RANGE:
    return snprintf(buf, buf_size, "Range");
  case TY_ASSOC: {
    // the base is a type parameter (`T.Item`) or a trait's abstract `Self` —
    // print it recursively rather than assuming a generic.
    int n = type_name_sprintf(t->as.assoc.base, buf, buf_size);
    if (n < 0 || (size_t)n >= buf_size) {
      return n;
    }
    return n + snprintf(buf + n, buf_size - n, "." SV_FMT,
                        SV_ARG(t->as.assoc.assoc_name));
  }
  }
  assert(false && "type_sprintf not implemented for non-singleton types");
  return 0;
}

// snprintf returns the length it *would* have written, so accumulating that
// unclamped walks `buf + n` past the end and underflows `buf_size - n` (a
// size_t) into a huge value — an out-of-bounds write for any type whose
// rendering exceeds the caller's buffer (conventionally char[64]). Clamp after
// every step so the running offset always leaves room for a NUL.
static int sp_bump(int n, size_t buf_size, int written) {
  if (written < 0) {
    return n; // encoding error
  }
  n += written;
  int limit = (int)buf_size - 1;
  return n > limit ? limit : n;
}

int type_sprintf(const Type *t, char *buf, size_t buf_size) {
  if (!t) {
    return snprintf(buf, buf_size, "NULL_TYPE");
  }
  switch (t->kind) {
  case TY_INT:
    return snprintf(buf, buf_size, "int");
  case TY_FLOAT:
    return snprintf(buf, buf_size, "float");
  case TY_BOOL:
    return snprintf(buf, buf_size, "bool");
  case TY_CHAR:
    return snprintf(buf, buf_size, "char");
  case TY_STRING:
    return snprintf(buf, buf_size, "String");
  case TY_STRBUF:
    return snprintf(buf, buf_size, "StringBuf");
  case TY_UNIT:
    return snprintf(buf, buf_size, "()");
  case TY_NEVER:
    return snprintf(buf, buf_size, "!");
  case TY_UNKNOWN:
    if (t->as.unknown.bound) {
      return type_sprintf(t->as.unknown.bound, buf, buf_size);
    } else {
      return snprintf(buf, buf_size, "_");
    }
  case TY_POISON:
    return snprintf(buf, buf_size, "<POISON>");
  case TY_GENERIC:
    return snprintf(buf, buf_size, SV_FMT,
                    SV_ARG(t->as.generic.name)); // todo: include bounds
  case TY_FUNCTION: {
    int n = sp_bump(0, buf_size, snprintf(buf, buf_size, "fun("));
    for (int i = 0; i < t->as.fun.param_count; i++) {
      if (i > 0)
        n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ", "));
      n = sp_bump(
          n, buf_size,
          type_sprintf(t->as.fun.param_types[i], buf + n, buf_size - n));
    }
    n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ") -> "));
    n = sp_bump(n, buf_size,
                type_sprintf(t->as.fun.return_type, buf + n, buf_size - n));
    return n;
  }
  case TY_TUPLE: {
    int n = sp_bump(0, buf_size, snprintf(buf, buf_size, "("));
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      if (i > 0)
        n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ", "));
      n = sp_bump(
          n, buf_size,
          type_sprintf(t->as.tuple.elem_types[i], buf + n, buf_size - n));
    }
    n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ")"));
    return n;
  }
  case TY_STRUCT: {
    int n =
        sp_bump(0, buf_size,
                snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.struc.def->name)));
    if (t->as.struc.type_arg_count == 0) {
      return n;
    }

    bool is_tuple_struct = t->as.struc.def->is_tuple;

    n = sp_bump(n, buf_size,
                snprintf(buf + n, buf_size - n, is_tuple_struct ? "(" : "<"));
    for (int i = 0; i < t->as.struc.type_arg_count; i++) {
      if (i > 0)
        n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ", "));
      n = sp_bump(
          n, buf_size,
          type_sprintf(t->as.struc.type_args[i], buf + n, buf_size - n));
    }
    n = sp_bump(n, buf_size,
                snprintf(buf + n, buf_size - n, is_tuple_struct ? ")" : ">"));
    return n;
  }
  case TY_ENUM: {
    int n =
        sp_bump(0, buf_size,
                snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.enm.def->name)));
    if (t->as.enm.type_arg_count > 0) {
      n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, "<"));
      for (int i = 0; i < t->as.enm.type_arg_count; i++) {
        if (i > 0)
          n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ", "));
        n = sp_bump(
            n, buf_size,
            type_sprintf(t->as.enm.type_args[i], buf + n, buf_size - n));
      }
      n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ">"));
    }
    return n;
  }
  case TY_TRAIT: {
    int n =
        sp_bump(0, buf_size,
                snprintf(buf, buf_size, SV_FMT, SV_ARG(t->as.trait.def->name)));
    // the type arguments are part of the identity, so a mismatch has to show
    // them: `Into<int>` and `Into<String>` are two bounds.
    for (int i = 0; i < t->as.trait.type_arg_count; i++) {
      n = sp_bump(n, buf_size,
                  snprintf(buf + n, buf_size - n, "%s", i == 0 ? "<" : ", "));
      n = sp_bump(
          n, buf_size,
          type_sprintf(t->as.trait.type_args[i], buf + n, buf_size - n));
    }
    if (t->as.trait.type_arg_count > 0) {
      n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ">"));
    }
    return n;
  }
  case TY_DYN: {
    const TraitDef *def = dyn_trait_def(t);
    const TypeTrait *ref = &t->as.dyn.trait->as.trait;
    int n = sp_bump(0, buf_size,
                    snprintf(buf, buf_size, "dyn " SV_FMT, SV_ARG(def->name)));
    // the type arguments and the bindings are both part of the type, so a
    // mismatch diagnostic has to show them: `dyn Iterator<Item = int>` and
    // `dyn Iterator<Item = String>` are two different expectations and would
    // otherwise print the same. One bracket list, arguments before bindings —
    // the same shape the source writes.
    int written = 0;
    for (int i = 0; i < ref->type_arg_count; i++) {
      n = sp_bump(
          n, buf_size,
          snprintf(buf + n, buf_size - n, "%s", written++ == 0 ? "<" : ", "));
      n = sp_bump(n, buf_size,
                  type_sprintf(ref->type_args[i], buf + n, buf_size - n));
    }
    for (int i = 0; i < t->as.dyn.assoc_type_count; i++) {
      // numbered over the closure, so a subtrait's table names its supers'
      // bindings and `def->assoc_types[i]` would be the wrong array.
      StringView name = trait_flat_assoc_name(def, i, NULL);
      n = sp_bump(n, buf_size,
                  snprintf(buf + n, buf_size - n, "%s" SV_FMT " = ",
                           written++ == 0 ? "<" : ", ", SV_ARG(name)));
      n = sp_bump(
          n, buf_size,
          type_sprintf(t->as.dyn.assoc_types[i], buf + n, buf_size - n));
    }
    if (written > 0) {
      n = sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, ">"));
    }
    return n;
  }
  case TY_ARRAY: {
    int n = sp_bump(0, buf_size, snprintf(buf, buf_size, "["));
    n = sp_bump(n, buf_size,
                type_sprintf(t->as.array.elem_type, buf + n, buf_size - n));
    return sp_bump(n, buf_size, snprintf(buf + n, buf_size - n, "]"));
  }
  case TY_RANGE:
    return snprintf(buf, buf_size, "Range");
  case TY_ASSOC: {
    // the base is a type parameter (`T.Item`) or the abstract `Self` of a
    // trait declaration — print it recursively rather than assuming a generic.
    int n = sp_bump(0, buf_size, type_sprintf(t->as.assoc.base, buf, buf_size));
    return sp_bump(n, buf_size,
                   snprintf(buf + n, buf_size - n, "." SV_FMT,
                            SV_ARG(t->as.assoc.assoc_name)));
  }
  }
  assert(false && "type_sprintf not implemented for non-singleton types");
  return 0;
}

Expr *ast_expr(ExprKind kind, Span span, Allocator *al) {
  Expr *e = al_alloc_zero_for(al, Expr);
  e->kind = kind;
  e->span = span;
  return e;
}

Stmt *ast_stmt(StmtKind kind, Span span, Allocator *al) {
  Stmt *s = al_alloc_zero_for(al, Stmt);
  s->kind = kind;
  s->span = span;
  return s;
}

Decl *ast_decl(DeclKind kind, Span span, Allocator *al) {
  Decl *d = al_alloc_zero_for(al, Decl);
  d->kind = kind;
  d->span = span;
  return d;
}

TypeNode *ast_type_node(TypeNodeKind kind, Span span, Allocator *al) {
  TypeNode *tn = al_alloc_zero_for(al, TypeNode);
  tn->kind = kind;
  tn->span = span;
  return tn;
}

static void ind(int indent) {
  for (int i = 0; i < indent; i++) {
    fprintf(stdout, "  ");
  }
}

// the node's own line, with its label appended when it has one. Callers have
// already indented, so this only finishes the line.
static void dump_label(LoopLabel label, const char *kind) {
  if (label.name.len > 0) {
    fprintf(stdout, "%s " SV_FMT "\n", kind, SV_ARG(label.name));
  } else {
    fprintf(stdout, "%s\n", kind);
  }
}

static void dump_typenode(const TypeNode *tn, int indent);

static void dump_path(const Path *path, int indent) {
  for (int i = 0; i < path->count; i++) {
    ind(indent);
    fprintf(stdout, "::" SV_FMT "\n", SV_ARG(path->segments[i].name));
    for (int j = 0; j < path->segments[i].type_arg_count; j++) {
      dump_typenode(path->segments[i].type_args[j], indent + 1);
    }
  }
}

static void dump_typenode(const TypeNode *tn, int indent);

void dump_type(const Type *t) {
  if (!t) {
    fprintf(stdout, "NULL_TYPE");
    return;
  }
  switch (t->kind) {
  case TY_INT:
    fprintf(stdout, "int");
    break;
  case TY_FLOAT:
    fprintf(stdout, "float");
    break;
  case TY_BOOL:
    fprintf(stdout, "bool");
    break;
  case TY_CHAR:
    fprintf(stdout, "char");
    break;
  case TY_STRING:
    fprintf(stdout, "String");
    break;
  case TY_STRBUF:
    fprintf(stdout, "StringBuf");
    break;
  case TY_UNIT:
    fprintf(stdout, "()");
    break;
  case TY_NEVER:
    fprintf(stdout, "!");
    break;
  case TY_UNKNOWN:
    fprintf(stdout, "<UNKNOWN>");
    break;
  case TY_POISON:
    fprintf(stdout, "<POISON>");
    break;
  case TY_GENERIC:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.generic.name));
    break;
  case TY_ASSOC:
    dump_type(t->as.assoc.base);
    fprintf(stdout, "." SV_FMT, SV_ARG(t->as.assoc.assoc_name));
    break;
  case TY_ARRAY:
    fprintf(stdout, "Array<");
    dump_type(t->as.array.elem_type);
    fprintf(stdout, ">");
    break;
  case TY_RANGE:
    fprintf(stdout, "Range");
    break;
  case TY_TUPLE:
    fprintf(stdout, "(");
    for (int i = 0; i < t->as.tuple.elem_count; i++) {
      dump_type(t->as.tuple.elem_types[i]);
      if (i < t->as.tuple.elem_count - 1)
        fprintf(stdout, ", ");
    }
    fprintf(stdout, ")");
    break;
  case TY_FUNCTION:
    fprintf(stdout, "fun(");
    for (int i = 0; i < t->as.fun.param_count; i++) {
      dump_type(t->as.fun.param_types[i]);
      if (i < t->as.fun.param_count - 1)
        fprintf(stdout, ", ");
    }
    fprintf(stdout, "): ");
    dump_type(t->as.fun.return_type);
    break;
  case TY_STRUCT:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.struc.def->name));
    if (t->as.struc.type_arg_count > 0) {
      fprintf(stdout, "<...>"); // Simplified
    }
    break;
  case TY_ENUM:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.enm.def->name));
    break;
  case TY_TRAIT:
    fprintf(stdout, SV_FMT, SV_ARG(t->as.trait.def->name));
    if (t->as.trait.type_arg_count > 0) {
      fprintf(stdout, "<...>");
    }
    break;
  case TY_DYN:
    fprintf(stdout, "dyn " SV_FMT, SV_ARG(dyn_trait_def(t)->name));
    if (t->as.dyn.assoc_type_count > 0) {
      fprintf(stdout, "<...>");
    }
    break;
  }
}

static void dump_typenode(const TypeNode *tn, int indent) {
  if (!tn)
    return;
  ind(indent);
  switch (tn->kind) {
  case TYNODE_UNIT:
    fprintf(stdout, "TypeNode: ()\n");
    break;
  case TYNODE_NEVER:
    fprintf(stdout, "TypeNode: !\n");
    break;
  case TYNODE_SELF:
    fprintf(stdout, "TypeNode: Self\n");
    break;
  case TYNODE_POISON:
    fprintf(stdout, "TypeNode: <POISON>\n");
    break;
  case TYNODE_NAMED:
    fprintf(stdout, "TypeNode: Named\n");
    dump_path(&tn->as.named.path, indent + 1);
    break;
  case TYNODE_DYN:
    fprintf(stdout, "TypeNode: Dyn\n");
    dump_path(&tn->as.dyn.path, indent + 1);
    for (int i = 0; i < tn->as.dyn.binding_count; i++) {
      ind(indent + 1);
      fprintf(stdout, SV_FMT " =\n", SV_ARG(tn->as.dyn.bindings[i].name));
      dump_typenode(tn->as.dyn.bindings[i].type, indent + 2);
    }
    break;
  case TYNODE_TUPLE:
    fprintf(stdout, "TypeNode: Tuple\n");
    for (int i = 0; i < tn->as.tuple.count; i++) {
      dump_typenode(tn->as.tuple.elems[i], indent + 1);
    }
    break;
  case TYNODE_FUN:
    fprintf(stdout, "TypeNode: Function\n");
    ind(indent + 1);
    fprintf(stdout, "Parameters:\n");
    for (int i = 0; i < tn->as.fun.param_count; i++) {
      dump_typenode(tn->as.fun.param_types[i], indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Returns:\n");
    dump_typenode(tn->as.fun.return_type, indent + 2);
    break;
  case TYNODE_ASSOC:
    fprintf(stdout, "TypeNode: Assoc(" SV_FMT ")\n",
            SV_ARG(tn->as.assoc.assoc_name));
    break;
  case TYNODE_ARRAY:
    fprintf(stdout, "TypeNode: Array\n");
    dump_typenode(tn->as.array.elem, indent + 1);
    break;
  }
  if (tn->resolved) {
    ind(indent + 1);
    fprintf(stdout, "Resolved Type:\n");
    ind(indent + 2);
    dump_type(tn->resolved);
    fprintf(stdout, "\n");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PATTERNS
// ═══════════════════════════════════════════════════════════════════════════════

void dump_pattern(const Pattern *p, int indent) {
  if (!p)
    return;
  ind(indent);
  switch (p->kind) {
  case PAT_WILDCARD:
    fprintf(stdout, "Pattern: _\n");
    break;
  case PAT_LITERAL:
    fprintf(stdout, "Pattern: Literal\n");
    dump_expr(p->as.literal_expr, indent + 1);
    break;
  case PAT_BIND:
    fprintf(stdout, "Pattern: Bind(" SV_FMT ")\n", SV_ARG(p->as.bind.name));
    break;
  case PAT_VARIANT:
    fprintf(stdout, "Pattern: Variant\n");
    dump_path(&p->as.variant.path, indent + 1);
    fprintf(stdout, ")\n");
    for (int i = 0; i < p->as.variant.field_count; i++) {
      ind(indent + 1);
      // fprintf(stdout, "Field: " SV_FMT "\n",
      //         SV_ARG(p->as.variant.fields[i].ident.name));
      if (p->as.variant.fields[i].sub_pattern) {
        dump_pattern(p->as.variant.fields[i].sub_pattern, indent + 2);
      }
    }
    break;
  case PAT_STRUCT:
    fprintf(stdout, "Pattern: Struct(");
    dump_path(&p->as.struc.path, indent + 1);
    fprintf(stdout, ")\n");
    for (int i = 0; i < p->as.struc.field_count; i++) {
      ind(indent + 1);
      // fprintf(stdout, "Field: " SV_FMT "\n",
      //         SV_ARG(p->as.struc.fields[i].ident.name));
      if (p->as.struc.fields[i].sub_pattern) {
        dump_pattern(p->as.struc.fields[i].sub_pattern, indent + 2);
      }
    }
    break;
  case PAT_TUPLE:
    fprintf(stdout, "Pattern: Tuple\n");
    for (int i = 0; i < p->as.tuple.count; i++) {
      dump_pattern(p->as.tuple.elems[i], indent + 1);
    }
    break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EXPRESSIONS
// ═══════════════════════════════════════════════════════════════════════════════

static void dump_bound(const TraitBound *bound, int indent) {
  if (bound->ref_count == 0)
    return;
  ind(indent);
  fprintf(stdout, "Bound:\n");
  for (int i = 0; i < bound->ref_count; i++) {
    ind(indent + 1);
    fprintf(stdout, "Ref: ");
    dump_path(&bound->refs[i].path, indent + 2);
  }
}

static void dump_where_clause(const WhereClause *where, int indent) {
  if (!where || where->pred_count == 0)
    return;
  ind(indent);
  fprintf(stdout, "Where Clause:\n");
  for (int i = 0; i < where->pred_count; i++) {
    ind(indent + 1);
    fprintf(stdout, "Predicate: ");
    for (int j = 0; j < where->preds[i].lhs.segment_count; j++) {
      fprintf(stdout, SV_FMT, SV_ARG(where->preds[i].lhs.segments[j]));
      if (j < where->preds[i].lhs.segment_count - 1)
        fprintf(stdout, ".");
    }
    fprintf(stdout, "\n");
    dump_bound(&where->preds[i].bound, indent + 2);
  }
}

void dump_expr(const Expr *e, int indent) {
  if (!e)
    return;
  ind(indent);

  switch (e->kind) {
  case EXPR_INT:
    fprintf(stdout, "int: %ld\n", e->as.int_val);
    break;
  case EXPR_FLOAT:
    fprintf(stdout, "float: %f\n", e->as.float_val);
    break;
  case EXPR_BOOL:
    fprintf(stdout, "bool: %s\n", e->as.bool_val ? "true" : "false");
    break;
  case EXPR_CHAR:
    fprintf(stdout, "char: U+%04X\n", e->as.char_val);
    break;
  case EXPR_STRING:
    fprintf(stdout, "String: \"" SV_FMT "\"\n", STR_ARG(e->as.string.value));
    break;
  case EXPR_UNIT:
    fprintf(stdout, "Unit: ()\n");
    break;
  case EXPR_POISON:
    fprintf(stdout, "<POISON EXPR>\n");
    break;
  case EXPR_SELF:
    fprintf(stdout, "Self\n");
    break;
  // case EXPR_VAR:
  //   fprintf(stdout, "Var: " SV_FMT "\n", SV_ARG(e->as.var.name));
  //   break;
  case EXPR_PATH:
    fprintf(stdout, "Path:\n");
    dump_path(&e->as.path_expr.path, indent + 1);
    break;
  case EXPR_BINARY:
    fprintf(stdout, "BinaryOp (%s)\n", token_type_to_string(e->as.binary.op));
    dump_expr(e->as.binary.left, indent + 1);
    dump_expr(e->as.binary.right, indent + 1);
    break;
  case EXPR_UNARY:
    fprintf(stdout, "UnaryOp (%s)\n", token_type_to_string(e->as.unary.op));
    dump_expr(e->as.unary.operand, indent + 1);
    break;
  case EXPR_ASSIGN:
    fprintf(stdout, "Assign\n");
    ind(indent + 1);
    fprintf(stdout, "Operator: %s\n", token_type_to_string(e->as.assign.op));
    dump_expr(e->as.assign.target, indent + 1);
    dump_expr(e->as.assign.value, indent + 1);
    break;
  case EXPR_CALL:
    fprintf(stdout, "Call\n");
    dump_expr(e->as.call.callee, indent + 1);
    ind(indent + 1);
    fprintf(stdout, "Arguments:\n");
    for (int i = 0; i < e->as.call.arg_count; i++) {
      dump_expr(e->as.call.args[i], indent + 2);
    }
    break;
  case EXPR_METHOD_CALL:
    fprintf(stdout, "MethodCall: ." SV_FMT "()\n",
            SV_ARG(e->as.method_call.method_name));
    dump_expr(e->as.method_call.object, indent + 1);
    for (int i = 0; i < e->as.method_call.arg_count; i++) {
      dump_expr(e->as.method_call.args[i], indent + 2);
    }
    break;
  case EXPR_BLOCK:
    dump_label(e->as.block.label, "Block");
    for (int i = 0; i < e->as.block.stmt_count; i++) {
      dump_stmt(e->as.block.stmts[i], indent + 1);
    }
    if (e->as.block.tail_expr) {
      dump_expr(e->as.block.tail_expr, indent + 1);
    }
    break;
  case EXPR_IF:
    fprintf(stdout, "If\n");
    if (e->as.if_expr.binding) {
      ind(indent + 1);
      fprintf(stdout, "Binding:\n");
      dump_pattern(e->as.if_expr.binding, indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Condition:\n");
    dump_expr(e->as.if_expr.condition, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Then:\n");
    dump_expr(e->as.if_expr.then_block, indent + 2);
    if (e->as.if_expr.else_branch) {
      ind(indent + 1);
      fprintf(stdout, "Else:\n");
      dump_expr(e->as.if_expr.else_branch, indent + 2);
    }
    break;
  case EXPR_MATCH:
    fprintf(stdout, "Match\n");
    dump_expr(e->as.match.subject, indent + 1);
    for (int i = 0; i < e->as.match.arm_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Arm:\n");
      dump_pattern(e->as.match.arms[i].pattern, indent + 2);
      if (e->as.match.arms[i].guard) {
        ind(indent + 2);
        fprintf(stdout, "Guard:\n");
        dump_expr(e->as.match.arms[i].guard, indent + 3);
      }
      ind(indent + 2);
      fprintf(stdout, "Body:\n");
      dump_expr(e->as.match.arms[i].body, indent + 3);
    }
    break;
  case EXPR_FIELD: {
    StringView field_name =
        e->as.field.is_tuple ? (StringView){} : e->as.field.ident.name;
    fprintf(stdout, "Field Access: ." SV_FMT "\n", SV_ARG(field_name));
    dump_expr(e->as.field.object, indent + 1);
    break;
  }
  case EXPR_STRUCT_INIT:
    fprintf(stdout, "StructInit:\n");
    dump_path(&e->as.struct_init.path, indent + 1);
    for (int i = 0; i < e->as.struct_init.field_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Field: " SV_FMT "\n",
              SV_ARG(e->as.struct_init.fields[i].ident.name));
      dump_expr(e->as.struct_init.fields[i].value, indent + 2);
    }
    break;
  case EXPR_TUPLE:
    fprintf(stdout, "Tuple\n");
    for (int i = 0; i < e->as.tuple.count; i++) {
      dump_expr(e->as.tuple.elems[i], indent + 1);
    }
    break;
  case EXPR_FOR: {
    dump_label(e->as.for_expr.label, "For");
    ind(indent + 1);
    fprintf(stdout, "Variable: " SV_FMT "\n", SV_ARG(e->as.for_expr.var_name));
    ind(indent + 1);
    fprintf(stdout, "Iterable:\n");
    dump_expr(e->as.for_expr.iterable, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.for_expr.body, indent + 2);
    break;
  }
  case EXPR_WHILE: {
    dump_label(e->as.while_expr.label, "While");
    if (e->as.while_expr.binding) {
      ind(indent + 1);
      fprintf(stdout, "Binding:\n");
      dump_pattern(e->as.while_expr.binding, indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Condition:\n");
    dump_expr(e->as.while_expr.condition, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.while_expr.body, indent + 2);
    break;
  }
  case EXPR_LOOP: {
    dump_label(e->as.loop_expr.label, "Loop");
    dump_expr(e->as.loop_expr.body, indent + 1);
    break;
  }
  case EXPR_RANGE:
    fprintf(stdout, "Range %s\n",
            e->as.range.inclusive ? "(inclusive)" : "(exclusive)");
    dump_expr(e->as.range.start, indent + 1);
    dump_expr(e->as.range.end, indent + 1);
    break;
  case EXPR_INTERPOLATED: {
    fprintf(stdout, "Interpolated String\n");
    for (int i = 0; i < e->as.interpolated.seg_count; i++) {
      if (e->as.interpolated.segs[i].kind == ISEG_EXPR) {
        ind(indent + 1);
        fprintf(stdout, "Expr Segment:\n");
        dump_expr(e->as.interpolated.segs[i].expr, indent + 2);
      } else {
        ind(indent + 1);
        fprintf(stdout, "String Segment: \"" SV_FMT "\"\n",
                STR_ARG(e->as.interpolated.segs[i].text));
      }
    }
    break;
  }
  case EXPR_INDEX: {
    fprintf(stdout, "Index\n");
    ind(indent + 1);
    fprintf(stdout, "Object:\n");
    dump_expr(e->as.index.object, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Index:\n");
    dump_expr(e->as.index.index, indent + 2);
    break;
  }
  case EXPR_CAST: {
    fprintf(stdout, "Cast\n");
    ind(indent + 1);
    fprintf(stdout, "Operand:\n");
    dump_expr(e->as.cast.operand, indent + 2);
    ind(indent + 1);
    fprintf(stdout, "Target Type:\n");
    dump_typenode(e->as.cast.target_type, indent + 2);
    break;
  }
  case EXPR_PROPAGATE: {
    fprintf(stdout, "Propagate\n");
    ind(indent + 1);
    fprintf(stdout, "Operand:\n");
    dump_expr(e->as.propagate.operand, indent + 2);
    break;
  }
  // case EXPR_ARRAY:
  case EXPR_VARIANT:
    fprintf(stdout, "Variant: ");
    dump_path(&e->as.variant.path, indent + 1);
    fprintf(stdout, "\n");
    // for (int i = 0; i < e->as.variant.payload_count; i++) {
    //   dump_expr(e->as.variant.payloads[i], indent + 1);
    // }
    break;
  case EXPR_CLOSURE:
    fprintf(stdout, "Closure\n");
    for (int i = 0; i < e->as.closure.param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Param: " SV_FMT "\n",
              SV_ARG(e->as.closure.params[i].name));
      if (e->as.closure.params[i].type_annotation) {
        dump_typenode(e->as.closure.params[i].type_annotation, indent + 2);
      }
    }
    if (e->as.closure.return_type_annotation) {
      ind(indent + 1);
      fprintf(stdout, "Returns:\n");
      dump_typenode(e->as.closure.return_type_annotation, indent + 2);
    }
    ind(indent + 1);
    fprintf(stdout, "Body:\n");
    dump_expr(e->as.closure.body, indent + 2);
    break;
  default:
    fprintf(stdout, "ExprKind: %d (Add remaining cases as needed)\n", e->kind);
    break;
  }
}

void dump_stmt(const Stmt *s, int indent) {
  if (!s)
    return;
  ind(indent);
  switch (s->kind) {
  case STMT_EXPR:
    fprintf(stdout, "ExprStmt\n");
    dump_expr(s->as.expr_stmt.expr, indent + 1);
    break;
  case STMT_VAR:
    fprintf(stdout, "VarStmt\n");
    dump_pattern(s->as.var_stmt.binding, indent + 1);
    if (s->as.var_stmt.type_annotation) {
      dump_typenode(s->as.var_stmt.type_annotation, indent + 1);
    }
    if (s->as.var_stmt.initializer) {
      dump_expr(s->as.var_stmt.initializer, indent + 1);
    }
    if (s->as.var_stmt.else_block) {
      ind(indent + 1);
      fprintf(stdout, "Else:\n");
      dump_expr(s->as.var_stmt.else_block, indent + 2);
    }
    break;
  case STMT_RETURN:
    fprintf(stdout, "ReturnStmt\n");
    if (s->as.return_stmt.value) {
      dump_expr(s->as.return_stmt.value, indent + 1);
    }
    break;
  case STMT_BREAK:
    dump_label(s->as.break_stmt.label, "BreakStmt");
    if (s->as.break_stmt.value) {
      dump_expr(s->as.break_stmt.value, indent + 1);
    }
    break;
  case STMT_CONTINUE:
    dump_label(s->as.continue_stmt.label, "ContinueStmt");
    break;
  case STMT_POISON:
    fprintf(stdout, "<POISON STMT>\n");
    break;
  }
}

void dump_decl(const Decl *d, int indent) {
  if (!d)
    return;
  ind(indent);
  switch (d->kind) {
  case DECL_FUN:
    fprintf(stdout, "FunDecl: " SV_FMT "\n", SV_ARG(d->as.fun_decl.name));
    if (d->as.fun_decl.attr.kind != ATTR_NONE) {
      ind(indent + 1);
      fprintf(stdout, "%s: " SV_FMT "\n",
              d->as.fun_decl.attr.kind == ATTR_NATIVE ? "Native" : "Intrinsic",
              SV_ARG(d->as.fun_decl.attr.name));
    }
    for (int i = 0; i < d->as.fun_decl.type_param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "TypeParam: " SV_FMT "\n",
              SV_ARG(d->as.fun_decl.type_params[i].name));
      dump_bound(&d->as.fun_decl.type_params[i].inline_bound, indent + 2);
    }
    for (int i = 0; i < d->as.fun_decl.param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Param: " SV_FMT "\n",
              SV_ARG(d->as.fun_decl.params[i].name));
      dump_typenode(d->as.fun_decl.params[i].type_annotation, indent + 2);
    }
    if (d->as.fun_decl.return_type) {
      ind(indent + 1);
      fprintf(stdout, "Returns:\n");
      dump_typenode(d->as.fun_decl.return_type, indent + 2);
    }
    dump_where_clause(d->as.fun_decl.where_clause, indent + 1);
    if (d->as.fun_decl.body) {
      dump_expr(d->as.fun_decl.body, indent + 1);
    }
    break;

  case DECL_STRUCT:
    fprintf(stdout, "StructDecl: " SV_FMT "\n", SV_ARG(d->as.struct_decl.name));
    if (d->as.struct_decl.is_tuple) {
      for (int i = 0; i < d->as.struct_decl.field_count; i++) {
        dump_typenode(d->as.struct_decl.fields[i].type_annotation, indent + 1);
      }
    } else {
      for (int i = 0; i < d->as.struct_decl.field_count; i++) {
        ind(indent + 1);
        fprintf(stdout, "Field: " SV_FMT "\n",
                SV_ARG(d->as.struct_decl.fields[i].ident.name));
        dump_typenode(d->as.struct_decl.fields[i].type_annotation, indent + 2);
      }
    }
    break;

  case DECL_ENUM:
    fprintf(stdout, "EnumDecl: " SV_FMT "\n", SV_ARG(d->as.enum_decl.name));
    for (int i = 0; i < d->as.enum_decl.variant_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "Variant: " SV_FMT "\n",
              SV_ARG(d->as.enum_decl.variants[i].name));
      if (d->as.enum_decl.variants[i].field_count == 0) {
        continue; // No payload
      }
      if (d->as.enum_decl.variants[i].is_tuple) {
        for (int j = 0; j < d->as.enum_decl.variants[i].field_count; j++) {
          dump_typenode(d->as.enum_decl.variants[i].fields[j].type_annotation,
                        indent + 2);
        }
      } else {
        for (int j = 0; j < d->as.enum_decl.variants[i].field_count; j++) {
          ind(indent + 2);
          fprintf(stdout, "Field: " SV_FMT "\n",
                  SV_ARG(d->as.enum_decl.variants[i].fields[j].ident.name));
          dump_typenode(d->as.enum_decl.variants[i].fields[j].type_annotation,
                        indent + 3);
        }
      }
    }
    break;

  case DECL_USE:
    fprintf(stdout, "UseDecl:\n");

    ind(indent + 1);
    printf("Path:\n");
    dump_path(&d->as.use_decl.path, indent + 2);

    ind(indent + 1);
    printf("Aliases:\n");
    for (int i = 0; i < d->as.use_decl.target.count; i++) {
      ind(indent + 2);
      fprintf(stdout, SV_FMT " as " SV_FMT "\n",
              SV_ARG(d->as.use_decl.target.aliases[i].name),
              SV_ARG(d->as.use_decl.target.aliases[i].alias));
    }
    break;

  case DECL_MOD:
    fprintf(stdout, "ModDecl: " SV_FMT "\n", SV_ARG(d->as.mod_decl.name));
    break;

  case DECL_VAR:
    fprintf(stdout, "VarDecl\n");
    dump_pattern(d->as.var_decl.binding, indent + 1);
    if (d->as.var_decl.initializer) {
      dump_expr(d->as.var_decl.initializer, indent + 1);
    }
    break;

  case DECL_TRAIT:
    fprintf(stdout, "TraitDecl: " SV_FMT "\n", SV_ARG(d->as.trait_decl.name));
    for (int i = 0; i < d->as.trait_decl.type_param_count; i++) {
      ind(indent + 1);
      fprintf(stdout, "TypeParam: " SV_FMT "\n",
              SV_ARG(d->as.trait_decl.type_params[i].name));
      dump_bound(&d->as.trait_decl.type_params[i].inline_bound, indent + 2);
    }
    if (d->as.trait_decl.supers.ref_count > 0) {
      ind(indent + 1);
      fprintf(stdout, "Supers:\n");
      dump_bound(&d->as.trait_decl.supers, indent + 2);
    }
    for (int i = 0; i < d->as.trait_decl.item_count; i++) {
      ind(indent + 2);
      fprintf(stdout, "Item: " SV_FMT "\n",
              SV_ARG(d->as.trait_decl.items[i].name));

      ind(indent + 3);
      if (d->as.trait_decl.items[i].kind == TRAIT_ITEM_METHOD) {
        fprintf(stdout, "MethodDecl: %s\n",
                d->as.trait_decl.items[i].default_body ? "with default impl"
                                                       : "");

        if (d->as.trait_decl.items[i].type_param_count > 0) {
          ind(indent + 4);
          fprintf(stdout, "Type Parameters:\n");
          for (int j = 0; j < d->as.trait_decl.items[i].type_param_count; j++) {
            ind(indent + 5);
            fprintf(stdout, "TypeParam: " SV_FMT "\n",
                    SV_ARG(d->as.trait_decl.items[i].type_params[j].name));
            dump_bound(&d->as.trait_decl.items[i].type_params[j].inline_bound,
                       indent + 6);
          }
        }

        if (d->as.trait_decl.items[i].param_count > 0) {
          ind(indent + 4);
          fprintf(stdout, "Parameters:\n");
          for (int j = 0; j < d->as.trait_decl.items[i].param_count; j++) {

            if (d->as.trait_decl.items[i].params[j].is_self) {
              ind(indent + 5);
              fprintf(stdout, "Param: self\n");
            } else {
              ind(indent + 5);
              fprintf(stdout, "Param: " SV_FMT "\n",
                      SV_ARG(d->as.trait_decl.items[i].params[j].name));
              dump_typenode(d->as.trait_decl.items[i].params[j].type_annotation,
                            indent + 6);
            }
          }
        }
      } else {
        fprintf(stdout, "Associated Type: " SV_FMT "\n",
                SV_ARG(d->as.trait_decl.items[i].name));
      }
    }
    break;

  case DECL_IMPL:
    fprintf(stdout, "ImplDecl\n");
    ind(indent + 1);
    fprintf(stdout, "For:\n");
    dump_typenode(d->as.impl_decl.self_type, indent + 2);
    if (d->as.impl_decl.type_param_count > 0) {
      ind(indent + 1);
      fprintf(stdout, "Type Parameters:\n");
      for (int i = 0; i < d->as.impl_decl.type_param_count; i++) {
        ind(indent + 2);
        fprintf(stdout, "TypeParam: " SV_FMT "\n",
                SV_ARG(d->as.impl_decl.type_params[i].name));
        dump_bound(&d->as.impl_decl.type_params[i].inline_bound, indent + 3);
      }
    }
    ind(indent + 1);
    fprintf(stdout, "Impls Trait:\n");
    dump_typenode(d->as.impl_decl.trait_type, indent + 2);
    if (d->as.impl_decl.item_count > 0) {
      ind(indent + 1);
      fprintf(stdout, "Items:\n");
      for (int i = 0; i < d->as.impl_decl.item_count; i++) {
        ind(indent + 2);
        fprintf(stdout, "Item: " SV_FMT "\n",
                SV_ARG(d->as.impl_decl.items[i].name));
        if (d->as.impl_decl.items[i].kind == IMPL_ITEM_METHOD) {
          dump_decl(d->as.impl_decl.items[i].fun_decl, indent + 3);
        } else {
          ind(indent + 3);
          fprintf(stdout, "Associated Type: " SV_FMT "\n",
                  SV_ARG(d->as.impl_decl.items[i].name));
          dump_typenode(d->as.impl_decl.items[i].assoc_type, indent + 4);
        }
      }
    }
    break;

  case DECL_POISON:
    fprintf(stdout, "<POISON DECL>\n");
    break;
  }
}

void dump_program(const Program *p, int indent) {
  if (!p)
    return;
  ind(indent);
  fprintf(stdout, "Program:\n");
  for (int i = 0; i < p->decl_count; i++) {
    dump_decl(p->decls[i], indent + 1);
  }
}