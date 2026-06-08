#pragma once

#include "ast.h"

typedef struct TypeChecker TypeChecker;
typedef struct TypeResolver TypeResolver;
typedef struct ValueScope ValueScope;

struct TypeChecker {
  Type *t_int, *t_float, *t_bool, *t_string, *t_unit, *t_poison;
};

typedef struct {
  StringView name;
  Type *type;
  int slot;         // local slot or global slot (< 0)
  bool is_captured; // set when a nested closure captures this binding
} VarEntry;

struct ValueScope {
  VarEntry *entries;
  int count;
  int cap;

  ValueScope *parent;
  bool is_fn_boundary; // true for the outermost scope of a fun/closure
  bool is_loop;        // true if directly inside a for/while body
  int next_slot;       // next slot to assign (advanced by vscope_define)
  Allocator *al;
};

// push a new scope. next_slot is inherited from parent unless is_fn_boundary.
ValueScope *vscope_push(ValueScope *parent, bool is_fn_boundary, bool is_loop,
                        Allocator *al);

// pop this scope, returning its parent. does not free (arena-allocated).
ValueScope *vscope_pop(ValueScope *scope);

// walk the parent chain.  sets *out_crossed_fn if a fn boundary was crossed.
// returns null if not found.
VarEntry *vscope_lookup(ValueScope *scope, StringView name,
                        bool *out_crossed_fn);

// define a new binding; assigns the next slot.  returns the assigned slot.
// emits a diagnostic and returns -1 if the name already exists in this scope.
int vscope_define(ValueScope *scope, StringView name, Type *type,
                  DiagBag *diags, Span span);