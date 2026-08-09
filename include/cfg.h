#pragma once

#include "allocator.h"
#include "ast.h"
#include "diag.h"

// The control-flow graph of one function body, and the questions answered over
// it. Runs after the checker has finished the body, which is the first moment
// every name in it has resolved to a binding — the graph does no name
// resolution of its own, it reads the `VarEntry *` the checker recorded.
//
// `params` are the bindings the signature introduced, in declaration order;
// each is a store of whatever the caller passed. A closure met on the way gets
// a graph of its own, and the bindings it captures are excluded from the answer
// in this one.
void cfg_check_body(Expr *body, VarEntry **params, int param_count,
                    DiagBag *diags, Allocator *al);
