#include "cfg.h"
#include "sema.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════════
// The graph
// ═══════════════════════════════════════════════════════════════════════════════
//
// One basic block per straight run of code, holding the *binding events* that
// run in it. The events are all this graph carries: an expression is walked for
// which locals it reads and stores, and nothing else about it is recorded, so a
// block is a list of (kind, local, span) and never a list of values.
//
// The builder is one walk in evaluation order — `scan_expr` in codegen.c is the
// same shape — with a cursor for the block being filled. A construct that
// branches ends the cursor's block and opens the ones its arms need. `cur ==
// CFG_NONE` is control having left (a `return`, a `break`): events are dropped
// and edges out of it are no-ops until the walk reaches somewhere reachable.

#define CFG_NONE (-1)

typedef enum {
  CFG_STORE, // a value put into the binding: a declaration, or a plain `=`
  CFG_READ,  // a use of what is in it
} CfgOpKind;

typedef struct {
  CfgOpKind kind;
  int local;
  Span span;
  // the store the *signature* makes. Nothing in the body put this value there,
  // so the report says who did.
  bool from_param;
} CfgOp;

typedef struct {
  CfgOp *ops;
  int op_count;
  int op_cap;

  int *succ;
  int succ_count;
  int succ_cap;

  bool reachable;
} CfgBlock;

// A `break`/`continue` target. A labelled block is one too — it takes a
// `break`, so it is an edge out of the middle of a body — but it has nothing to
// continue to, which is the whole of the difference here.
typedef struct CfgLoop {
  struct CfgLoop *parent;
  StringView label;
  bool is_loop;
  int break_target;
  int continue_target; // CFG_NONE for a labelled block
} CfgLoop;

typedef struct Cfg {
  Allocator *al;
  DiagBag *diags;

  CfgBlock *blocks;
  int block_count;
  int block_cap;

  // dense index → binding, which is both how an event names one and how a
  // liveness bit is numbered. Only the bindings this lint could report are in
  // here: `self` and a `_` name are left out, so nothing about them is tracked
  // and no store into one is ever weighed.
  VarEntry **locals;
  bool *escaped;
  int local_count;
  int local_cap;

  int cur;
  int exit;
  CfgLoop *loops;

  // the graph of the enclosing function, when this one is a closure's. A name
  // this body reads that belongs out there is a capture.
  struct Cfg *parent;
} Cfg;

static void cfg_expr(Cfg *c, Expr *e);
static void cfg_stmt(Cfg *c, Stmt *s);
static void cfg_run(Cfg *c, Expr *body, VarEntry **params, int param_count);

static int cfg_new_block(Cfg *c) {
  if (c->block_count >= c->block_cap) {
    int new_cap = c->block_cap == 0 ? 8 : c->block_cap * 2;
    c->blocks =
        al_realloc(c->al, c->blocks, sizeof(CfgBlock) * (size_t)c->block_cap,
                   sizeof(CfgBlock) * (size_t)new_cap);
    assert(c->blocks && "out of memory");
    c->block_cap = new_cap;
  }
  c->blocks[c->block_count] = (CfgBlock){0};
  return c->block_count++;
}

static void cfg_edge(Cfg *c, int from, int to) {
  if (from == CFG_NONE || to == CFG_NONE) {
    return;
  }
  CfgBlock *b = &c->blocks[from];
  if (b->succ_count >= b->succ_cap) {
    int new_cap = b->succ_cap == 0 ? 2 : b->succ_cap * 2;
    b->succ = al_realloc(c->al, b->succ, sizeof(int) * (size_t)b->succ_cap,
                         sizeof(int) * (size_t)new_cap);
    assert(b->succ && "out of memory");
    b->succ_cap = new_cap;
  }
  b->succ[b->succ_count++] = to;
}

// The index of a binding, or -1 for one this graph does not track. A name that
// belongs to an enclosing body is a capture: it is marked there and answered
// nowhere, since a closure runs at a time this graph cannot see.
static int cfg_local(Cfg *c, VarEntry *ve) {
  if (ve == NULL) {
    return -1;
  }
  for (int i = 0; i < c->local_count; i++) {
    if (c->locals[i] == ve) {
      return i;
    }
  }
  for (Cfg *up = c->parent; up != NULL; up = up->parent) {
    for (int i = 0; i < up->local_count; i++) {
      if (up->locals[i] == ve) {
        up->escaped[i] = true;
        return -1;
      }
    }
  }
  return -1;
}

// A binding this lint can speak about. `self` is written by the signature and a
// `_` name is the author saying "bound on purpose, read never" — both are
// exempt at the declaration grain already, so neither is tracked here.
static bool cfg_reportable(VarEntry *ve) {
  return ve != NULL && ve->name.len > 0 && ve->name.chars[0] != '_' &&
         !sv_equal(ve->name, sv_from_cstr("self"));
}

static int cfg_declare(Cfg *c, VarEntry *ve) {
  if (!cfg_reportable(ve)) {
    return -1;
  }
  if (c->local_count >= c->local_cap) {
    int new_cap = c->local_cap == 0 ? 8 : c->local_cap * 2;
    c->locals =
        al_realloc(c->al, c->locals, sizeof(VarEntry *) * (size_t)c->local_cap,
                   sizeof(VarEntry *) * (size_t)new_cap);
    c->escaped =
        al_realloc(c->al, c->escaped, sizeof(bool) * (size_t)c->local_cap,
                   sizeof(bool) * (size_t)new_cap);
    assert(c->locals && c->escaped && "out of memory");
    c->local_cap = new_cap;
  }
  c->escaped[c->local_count] = false;
  c->locals[c->local_count] = ve;
  return c->local_count++;
}

static void cfg_emit_param(Cfg *c, CfgOpKind kind, int local, Span span,
                           bool from_param) {
  if (c->cur == CFG_NONE || local < 0) {
    return;
  }
  CfgBlock *b = &c->blocks[c->cur];
  if (b->op_count >= b->op_cap) {
    int new_cap = b->op_cap == 0 ? 4 : b->op_cap * 2;
    b->ops = al_realloc(c->al, b->ops, sizeof(CfgOp) * (size_t)b->op_cap,
                        sizeof(CfgOp) * (size_t)new_cap);
    assert(b->ops && "out of memory");
    b->op_cap = new_cap;
  }
  b->ops[b->op_count++] = (CfgOp){
      .kind = kind, .local = local, .span = span, .from_param = from_param};
}

static void cfg_emit(Cfg *c, CfgOpKind kind, int local, Span span) {
  cfg_emit_param(c, kind, local, span, false);
}

// Open a block and make it the cursor, entered from wherever the cursor was.
static int cfg_open(Cfg *c, int from) {
  int b = cfg_new_block(c);
  cfg_edge(c, from, b);
  c->cur = b;
  return b;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Building
// ═══════════════════════════════════════════════════════════════════════════════

// Every binding a pattern introduces is a store: the value came from the
// subject being taken apart, and overwriting it before reading is the same
// mistake as overwriting a `var`'s initializer.
static void cfg_pattern(Cfg *c, Pattern *p) {
  if (p == NULL) {
    return;
  }
  switch (p->kind) {
  case PAT_WILDCARD:
  case PAT_LITERAL:
    break;
  case PAT_BIND:
    cfg_emit(c, CFG_STORE, cfg_declare(c, p->as.bind.entry), p->span);
    break;
  case PAT_TUPLE:
    for (int i = 0; i < p->as.tuple.count; i++) {
      cfg_pattern(c, p->as.tuple.elems[i]);
    }
    break;
  case PAT_STRUCT:
  case PAT_VARIANT: {
    FieldPat *fields =
        p->kind == PAT_STRUCT ? p->as.struc.fields : p->as.variant.fields;
    int count = p->kind == PAT_STRUCT ? p->as.struc.field_count
                                      : p->as.variant.field_count;
    for (int i = 0; i < count; i++) {
      if (fields[i].sub_pattern != NULL) {
        cfg_pattern(c, fields[i].sub_pattern);
      } else {
        cfg_emit(c, CFG_STORE, cfg_declare(c, fields[i].entry), fields[i].span);
      }
    }
    break;
  }
  }
}

// `break` unlabelled names the innermost *loop*, skipping any labelled block
// between; labelled it names whatever declared the name. `continue` only ever
// names a loop, since a block has nowhere to continue to.
static CfgLoop *cfg_find_loop(Cfg *c, LoopLabel label, bool need_continue) {
  for (CfgLoop *l = c->loops; l != NULL; l = l->parent) {
    if (need_continue && !l->is_loop) {
      continue;
    }
    if (label.name.len > 0) {
      if (sv_equal(l->label, label.name)) {
        return l;
      }
      continue;
    }
    if (l->is_loop) {
      return l;
    }
  }
  return NULL;
}

static void cfg_block_expr(Cfg *c, Expr *e) {
  ExprBlock *blk = &e->as.block;
  int join = CFG_NONE;
  CfgLoop frame;
  if (blk->label.name.len > 0) {
    join = cfg_new_block(c);
    frame = (CfgLoop){.parent = c->loops,
                      .label = blk->label.name,
                      .is_loop = false,
                      .break_target = join,
                      .continue_target = CFG_NONE};
    c->loops = &frame;
  }

  for (int i = 0; i < blk->stmt_count; i++) {
    cfg_stmt(c, blk->stmts[i]);
  }
  cfg_expr(c, blk->tail_expr);

  if (join != CFG_NONE) {
    c->loops = frame.parent;
    cfg_edge(c, c->cur, join);
    c->cur = join;
  }
}

static void cfg_if_expr(Cfg *c, Expr *e) {
  ExprIf *i = &e->as.if_expr;
  cfg_expr(c, i->condition);
  int test = c->cur;

  cfg_open(c, test);
  cfg_pattern(c, i->binding); // an `if var`'s names live in the then branch
  cfg_expr(c, i->then_block);
  int after_then = c->cur;

  c->cur = test;
  cfg_open(c, test);
  cfg_expr(c, i->else_branch);
  int after_else = c->cur;

  int join = cfg_new_block(c);
  cfg_edge(c, after_then, join);
  cfg_edge(c, after_else, join);
  c->cur = join;
}

static void cfg_while_expr(Cfg *c, Expr *e) {
  ExprWhile *w = &e->as.while_expr;
  int header = cfg_open(c, c->cur);
  cfg_expr(c, w->condition);
  int test = c->cur;

  int exit_b = cfg_new_block(c);
  cfg_edge(c, test, exit_b);

  cfg_open(c, test);
  cfg_pattern(c, w->binding);
  CfgLoop frame = {.parent = c->loops,
                   .label = w->label.name,
                   .is_loop = true,
                   .break_target = exit_b,
                   .continue_target = header};
  c->loops = &frame;
  cfg_expr(c, w->body);
  c->loops = frame.parent;

  cfg_edge(c, c->cur, header); // the back edge is what makes this a fixpoint
  c->cur = exit_b;
}

static void cfg_loop_expr(Cfg *c, Expr *e) {
  ExprLoop *l = &e->as.loop_expr;
  int header = cfg_open(c, c->cur);
  int exit_b = cfg_new_block(c); // reachable only from a `break`, as written

  CfgLoop frame = {.parent = c->loops,
                   .label = l->label.name,
                   .is_loop = true,
                   .break_target = exit_b,
                   .continue_target = header};
  c->loops = &frame;
  cfg_expr(c, l->body);
  c->loops = frame.parent;

  cfg_edge(c, c->cur, header);
  c->cur = exit_b;
}

static void cfg_for_expr(Cfg *c, Expr *e) {
  ExprFor *f = &e->as.for_expr;
  cfg_expr(c, f->iterable);

  int header = cfg_open(c, c->cur);
  cfg_expr(c, f->next_call); // NULL for an array or a range
  int test = c->cur;

  int exit_b = cfg_new_block(c);
  cfg_edge(c, test, exit_b);

  cfg_open(c, test);
  cfg_emit(c, CFG_STORE, cfg_declare(c, f->var_entry), f->var_span);
  CfgLoop frame = {.parent = c->loops,
                   .label = f->label.name,
                   .is_loop = true,
                   .break_target = exit_b,
                   .continue_target = header};
  c->loops = &frame;
  cfg_expr(c, f->body);
  c->loops = frame.parent;

  cfg_edge(c, c->cur, header);
  c->cur = exit_b;
}

static void cfg_match_expr(Cfg *c, Expr *e) {
  ExprMatch *m = &e->as.match;
  cfg_expr(c, m->subject);
  int test = c->cur;
  int join = cfg_new_block(c);

  // The arms are a chain, not a fan: an arm is reached because every arm above
  // it declined, and an arm declines in two places — its pattern did not match,
  // or its guard said no after the bindings were already made. Both edges lead
  // to the same next test, which is what keeps a guard's effects visible to the
  // arms below it.
  for (int i = 0; i < m->arm_count; i++) {
    cfg_open(c, test);
    cfg_pattern(c, m->arms[i].pattern);

    int guard_fail = CFG_NONE;
    if (m->arms[i].guard != NULL) {
      cfg_expr(c, m->arms[i].guard);
      guard_fail = c->cur;
      cfg_open(c, guard_fail); // the body runs where the guard said yes
    }

    cfg_expr(c, m->arms[i].body);
    cfg_edge(c, c->cur, join);

    if (i + 1 < m->arm_count) {
      int next = cfg_new_block(c);
      cfg_edge(c, test, next);
      cfg_edge(c, guard_fail, next);
      test = next;
    }
  }
  c->cur = join;
}

// `a and b` / `a or b`: the right side runs only if the left did not decide it.
static void cfg_shortcircuit(Cfg *c, Expr *e) {
  cfg_expr(c, e->as.binary.left);
  int test = c->cur;
  cfg_open(c, test);
  cfg_expr(c, e->as.binary.right);
  int after_right = c->cur;

  int join = cfg_new_block(c);
  cfg_edge(c, after_right, join);
  cfg_edge(c, test, join);
  c->cur = join;
}

static void cfg_assign_expr(Cfg *c, Expr *e) {
  ExprAssign *a = &e->as.assign;
  // Every assignment into a bare name is a store here, where the checker's
  // `write_target` counts only a plain `=`: at this grain a compound `a op= b`
  // is a read *and* a store, so `counted += 1` with nothing after it is a dead
  // one — which is the case the declaration grain could not see.
  bool bare =
      a->target->kind == EXPR_PATH && a->target->as.path_expr.path.count == 1;
  if (!bare || a->op != TOKEN_EQ) {
    cfg_expr(c, a->target); // the old value, which every other form consults
  }
  cfg_expr(c, a->value);
  cfg_expr(c, a->op_call);

  if (bare) {
    cfg_emit(c, CFG_STORE, cfg_local(c, a->target->as.path_expr.resolved_local),
             e->span);
  }
}

static void cfg_expr(Cfg *c, Expr *e) {
  if (e == NULL) {
    return;
  }
  switch (e->kind) {
  case EXPR_INT:
  case EXPR_FLOAT:
  case EXPR_BOOL:
  case EXPR_STRING:
  case EXPR_CHAR:
  case EXPR_UNIT:
  case EXPR_SELF:
  case EXPR_VAR: // a bare name is an EXPR_PATH; this kind is unbuilt
  case EXPR_POISON:
    break;

  case EXPR_PATH:
    cfg_emit(c, CFG_READ, cfg_local(c, e->as.path_expr.resolved_local),
             e->span);
    break;

  case EXPR_INTERPOLATED:
    for (int i = 0; i < e->as.interpolated.seg_count; i++) {
      cfg_expr(c, e->as.interpolated.segs[i].expr);
    }
    break;

  case EXPR_BINARY:
    if (e->as.binary.op == TOKEN_AND || e->as.binary.op == TOKEN_OR) {
      cfg_shortcircuit(c, e);
    } else {
      cfg_expr(c, e->as.binary.left);
      cfg_expr(c, e->as.binary.right);
    }
    break;
  case EXPR_UNARY:
    cfg_expr(c, e->as.unary.operand);
    break;
  case EXPR_ASSIGN:
    cfg_assign_expr(c, e);
    break;
  case EXPR_RANGE:
    cfg_expr(c, e->as.range.start);
    cfg_expr(c, e->as.range.end);
    break;

  case EXPR_CALL:
    cfg_expr(c, e->as.call.callee);
    for (int i = 0; i < e->as.call.arg_count; i++) {
      cfg_expr(c, e->as.call.args[i]);
    }
    break;
  case EXPR_INDEX:
    cfg_expr(c, e->as.index.object);
    cfg_expr(c, e->as.index.index);
    break;
  case EXPR_FIELD:
    cfg_expr(c, e->as.field.object);
    break;
  case EXPR_METHOD_CALL:
    cfg_expr(c, e->as.method_call.object);
    for (int i = 0; i < e->as.method_call.arg_count; i++) {
      cfg_expr(c, e->as.method_call.args[i]);
    }
    break;
  case EXPR_CAST:
    cfg_expr(c, e->as.cast.operand);
    break;

  case EXPR_PROPAGATE: {
    // an `Err` leaves by the same door a `return` does, so this is a branch
    // whose other arm carries on with the `Ok`
    cfg_expr(c, e->as.propagate.operand);
    int test = c->cur;
    cfg_edge(c, test, c->exit);
    cfg_open(c, test);
    break;
  }

  case EXPR_BLOCK:
    cfg_block_expr(c, e);
    break;
  case EXPR_IF:
    cfg_if_expr(c, e);
    break;
  case EXPR_WHILE:
    cfg_while_expr(c, e);
    break;
  case EXPR_LOOP:
    cfg_loop_expr(c, e);
    break;
  case EXPR_FOR:
    cfg_for_expr(c, e);
    break;
  case EXPR_MATCH:
    cfg_match_expr(c, e);
    break;

  case EXPR_TUPLE:
    for (int i = 0; i < e->as.tuple.count; i++) {
      cfg_expr(c, e->as.tuple.elems[i]);
    }
    break;
  case EXPR_ARRAY:
    for (int i = 0; i < e->as.array.count; i++) {
      cfg_expr(c, e->as.array.elems[i]);
    }
    break;
  case EXPR_STRUCT_INIT:
    for (int i = 0; i < e->as.struct_init.field_count; i++) {
      cfg_expr(c, e->as.struct_init.fields[i].value);
    }
    break;
  case EXPR_VARIANT:
    for (int i = 0; i < e->as.variant.field_count; i++) {
      cfg_expr(c, e->as.variant.fields[i].value);
    }
    break;

  case EXPR_CLOSURE: {
    // A closure runs at a time no graph of this body can name, and an upvalue
    // is shared rather than copied — so every binding it captures is marked
    // out of the answer, and its own body is a graph in its own right.
    ExprClosure *cl = &e->as.closure;
    VarEntry **params =
        cl->param_count > 0
            ? al_alloc(c->al, sizeof(VarEntry *) * (size_t)cl->param_count)
            : NULL;
    for (int i = 0; i < cl->param_count; i++) {
      params[i] = cl->params[i].entry;
    }
    Cfg inner = {.al = c->al, .diags = c->diags, .parent = c};
    cfg_run(&inner, cl->body, params, cl->param_count);
    break;
  }
  }
}

static void cfg_stmt(Cfg *c, Stmt *s) {
  if (s == NULL) {
    return;
  }
  switch (s->kind) {
  case STMT_EXPR:
    cfg_expr(c, s->as.expr_stmt.expr);
    break;

  case STMT_VAR: {
    StmtVar *v = &s->as.var_stmt;
    cfg_expr(c, v->initializer);
    if (v->else_block != NULL) {
      // the pattern is refutable: `else` is where it did not match, and it has
      // to diverge, so nothing joins back
      int test = c->cur;
      cfg_open(c, test);
      cfg_expr(c, v->else_block);
      c->cur = test;
      cfg_open(c, test);
    }
    cfg_pattern(c, v->binding);
    break;
  }

  case STMT_RETURN:
    cfg_expr(c, s->as.return_stmt.value);
    cfg_edge(c, c->cur, c->exit);
    c->cur = CFG_NONE;
    break;

  case STMT_BREAK: {
    cfg_expr(c, s->as.break_stmt.value);
    CfgLoop *l = cfg_find_loop(c, s->as.break_stmt.label, false);
    cfg_edge(c, c->cur, l != NULL ? l->break_target : c->exit);
    c->cur = CFG_NONE;
    break;
  }

  case STMT_CONTINUE: {
    CfgLoop *l = cfg_find_loop(c, s->as.continue_stmt.label, true);
    cfg_edge(c, c->cur, l != NULL ? l->continue_target : c->exit);
    c->cur = CFG_NONE;
    break;
  }

  case STMT_POISON:
    break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Liveness, and the stores it condemns
// ═══════════════════════════════════════════════════════════════════════════════

typedef uint64_t Word;
#define WORD_BITS 64

static bool bits_get(const Word *w, int i) {
  return (w[i / WORD_BITS] >> (i % WORD_BITS)) & 1u;
}
static void bits_set(Word *w, int i) {
  w[i / WORD_BITS] |= (Word)1 << (i % WORD_BITS);
}
static void bits_clear(Word *w, int i) {
  w[i / WORD_BITS] &= ~((Word)1 << (i % WORD_BITS));
}

static void cfg_mark_reachable(Cfg *c, int b) {
  if (b == CFG_NONE || c->blocks[b].reachable) {
    return;
  }
  c->blocks[b].reachable = true;
  for (int i = 0; i < c->blocks[b].succ_count; i++) {
    cfg_mark_reachable(c, c->blocks[b].succ[i]);
  }
}

// Backward liveness to a fixpoint: a binding is live at a point when some path
// from it reads the binding before storing into it again. A loop is why this
// iterates — the back edge means a block's answer depends on one that has not
// been computed yet.
static void cfg_report(Cfg *c, int entry) {
  if (c->local_count == 0) {
    return;
  }
  cfg_mark_reachable(c, entry);

  int words = (c->local_count + WORD_BITS - 1) / WORD_BITS;
  Word *live_in = al_alloc_zero(c->al, sizeof(Word) * (size_t)words *
                                           (size_t)c->block_count);
  Word *scratch = al_alloc_zero(c->al, sizeof(Word) * (size_t)words);

  bool changed = true;
  while (changed) {
    changed = false;
    for (int b = c->block_count - 1; b >= 0; b--) {
      CfgBlock *blk = &c->blocks[b];
      memset(scratch, 0, sizeof(Word) * (size_t)words);
      for (int s = 0; s < blk->succ_count; s++) {
        Word *in = &live_in[(size_t)blk->succ[s] * (size_t)words];
        for (int w = 0; w < words; w++) {
          scratch[w] |= in[w];
        }
      }
      for (int i = blk->op_count - 1; i >= 0; i--) {
        if (blk->ops[i].kind == CFG_STORE) {
          bits_clear(scratch, blk->ops[i].local);
        } else {
          bits_set(scratch, blk->ops[i].local);
        }
      }
      Word *mine = &live_in[(size_t)b * (size_t)words];
      for (int w = 0; w < words; w++) {
        if (mine[w] != scratch[w]) {
          mine[w] = scratch[w];
          changed = true;
        }
      }
    }
  }

  int op_total = 0;
  for (int b = 0; b < c->block_count; b++) {
    op_total += c->blocks[b].op_count;
  }
  CfgOp *dead =
      op_total > 0 ? al_alloc(c->al, sizeof(CfgOp) * (size_t)op_total) : NULL;
  int dead_count = 0;

  // A binding nothing reads *anywhere* is one mistake with one report, made at
  // the declaration by `vscope_pop`; naming each of its stores as well would be
  // the same news several times. So this speaks only where the binding is
  // wanted and a particular store into it is not.
  for (int b = 0; b < c->block_count; b++) {
    CfgBlock *blk = &c->blocks[b];
    if (!blk->reachable) {
      continue; // `unreachable_code` already has this one
    }
    memset(scratch, 0, sizeof(Word) * (size_t)words);
    for (int s = 0; s < blk->succ_count; s++) {
      Word *in = &live_in[(size_t)blk->succ[s] * (size_t)words];
      for (int w = 0; w < words; w++) {
        scratch[w] |= in[w];
      }
    }
    for (int i = blk->op_count - 1; i >= 0; i--) {
      CfgOp *op = &blk->ops[i];
      if (op->kind == CFG_READ) {
        bits_set(scratch, op->local);
        continue;
      }
      VarEntry *ve = c->locals[op->local];
      if (!bits_get(scratch, op->local) && ve->used && !c->escaped[op->local]) {
        dead[dead_count++] = *op;
      }
      bits_clear(scratch, op->local);
    }
  }

  // block order is the order the *builder* opened them, which a loop or a
  // match interleaves — so the reader gets them sorted rather than that.
  for (int i = 1; i < dead_count; i++) {
    CfgOp hold = dead[i];
    int j = i - 1;
    while (j >= 0 && (dead[j].span.line > hold.span.line ||
                      (dead[j].span.line == hold.span.line &&
                       dead[j].span.col > hold.span.col))) {
      dead[j + 1] = dead[j];
      j--;
    }
    dead[j + 1] = hold;
  }
  for (int i = 0; i < dead_count; i++) {
    diag_warning(c->diags, LINT_UNUSED_ASSIGNMENT, dead[i].span,
                 dead[i].from_param
                     ? "value passed to '" SV_FMT "' is never read"
                     : "value assigned to '" SV_FMT "' is never read",
                 SV_ARG(c->locals[dead[i].local]->name));
    diag_note(c->diags, (Span){0}, "prefix it with '_' if that is deliberate");
  }
}

static void cfg_run(Cfg *c, Expr *body, VarEntry **params, int param_count) {
  c->exit = cfg_new_block(c);
  int entry = cfg_open(c, CFG_NONE);

  // a parameter is a binding the caller filled, so the signature is its store
  for (int i = 0; i < param_count; i++) {
    int local = cfg_declare(c, params[i]);
    cfg_emit_param(c, CFG_STORE, local,
                   params[i] != NULL ? params[i]->span : (Span){0}, true);
  }

  cfg_expr(c, body);
  cfg_edge(c, c->cur, c->exit);

  cfg_report(c, entry);
}

void cfg_check_body(Expr *body, VarEntry **params, int param_count,
                    DiagBag *diags, Allocator *al) {
  if (body == NULL) {
    return;
  }
  Cfg c = {.al = al, .diags = diags};
  cfg_run(&c, body, params, param_count);
}
