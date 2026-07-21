#include "codegen.h"
#include "ast.h"
#include "chunk.h"
#include "object.h"
#include "scanner.h"
#include "string_utils.h"
#include "value.h"

#include <assert.h>
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
  Module *m;
  Heap *heap;
  Chunk *chunk;
  DiagBag *diags;
  Allocator *al;

  CgLocal locals[CG_MAX_LOCALS];
  int local_count;

  CgUpvalue upvalues[CG_MAX_UPVALUES];
  int upvalue_count;

  struct Cg *parent; // enclosing function while compiling a closure; else NULL

  CgLoop *loop;
  bool ok;

  int self_slot; // -1 outside a method; the frame slot holding `self`
} Cg;

static void cg_error(Cg *cg, Span span, const char *what) {
  diag_error(cg->diags, span, "%s is not supported by the VM yet", what);
  cg->ok = false;
}

// ── emit helpers ─────────────────────────────────────────────────────────────

static void emit(Cg *cg, uint8_t byte) { chunk_write(cg->chunk, byte); }

static void emit2(Cg *cg, uint8_t a, uint8_t b) {
  emit(cg, a);
  emit(cg, b);
}

static void emit_const(Cg *cg, Value v) {
  emit2(cg, OP_CONST, (uint8_t)chunk_add_const(cg->chunk, v));
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
// rooted (closures aren't in m->funs/m->methods).
static void cg_register_closure(Cg *cg, FunDef *fun) {
  Module *m = cg->m;
  if (m->closure_count >= m->closure_cap) {
    int new_cap = m->closure_cap == 0 ? 4 : m->closure_cap * 2;
    m->closures = al_realloc(cg->al, m->closures,
                             sizeof(FunDef *) * (size_t)m->closure_cap,
                             sizeof(FunDef *) * (size_t)new_cap);
    m->closure_cap = new_cap;
  }
  m->closures[m->closure_count++] = fun;
}

static FunDef *cg_find_module_fun(Cg *cg, StringView name) {
  for (int i = 0; i < cg->m->fun_count; i++) {
    if (sv_equal(cg->m->funs[i]->name, name)) {
      return cg->m->funs[i];
    }
  }
  return NULL;
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

static void compile_stmt(Cg *cg, Stmt *stmt) {
  switch (stmt->kind) {
  case STMT_EXPR:
    compile_expr(cg, stmt->as.expr_stmt.expr);
    emit(cg, OP_POP);
    break;

  case STMT_VAR: {
    StmtVar *var = &stmt->as.var_stmt;
    if (var->binding.kind != BIND_IDENT) {
      cg_error(cg, stmt->span, "destructuring binding");
      break;
    }
    compile_expr(cg, var->initializer);
    // the initializer's stack slot becomes the local
    cg_add_local(cg, var->binding.as.ident, stmt->span);
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

static void compile_assign(Cg *cg, Expr *expr) {
  ExprAssign *assign = &expr->as.assign;

  if (assign->target->kind == EXPR_INDEX) {
    compile_index_assign(cg, expr);
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

static void compile_for(Cg *cg, Expr *expr) {
  ExprFor *for_ = &expr->as.for_expr;
  Type *iter_ty = for_->iterable->resolved_type;
  assert(iter_ty && "for-loop iterable unresolved after checking");
  if (iter_ty->kind == TY_ARRAY) {
    compile_for_array(cg, expr);
  } else {
    compile_for_range(cg, expr);
  }
}

static void compile_call(Cg *cg, Expr *expr) {
  ExprCall *call = &expr->as.call;
  Expr *callee = call->callee;

  // builtin print: lowered to OP_PRINT when the name isn't shadowed
  if (callee->kind == EXPR_PATH && callee->as.path_expr.path.count == 1) {
    StringView name = callee->as.path_expr.path.segments[0].name;
    if (sv_equal_cstr(name, "print") && cg_find_local(cg, name) < 0 &&
        cg_find_module_fun(cg, name) == NULL) {
      assert(call->arg_count == 1 && "print arity got past the checker");
      compile_expr(cg, call->args[0]);
      emit(cg, OP_PRINT);
      return;
    }
  }

  compile_expr(cg, callee);
  for (int i = 0; i < call->arg_count; i++) {
    compile_expr(cg, call->args[i]);
  }
  emit2(cg, OP_CALL, (uint8_t)call->arg_count);
}

// `obj.method(args)`: the checker elides `self` from `mc->args` (it's
// `mc->object` instead), matching it against whichever param position in
// `fun->params` actually has `is_self` set (checked generically, not
// assumed to be 0 — see resolve_method_call_expr) — so codegen interleaves
// `mc->object` back in at that same position when pushing arguments.
static void compile_method_call(Cg *cg, Expr *expr) {
  ExprMethodCall *mc = &expr->as.method_call;
  FunDef *fun = mc->resolved_method->fun;

  emit2(cg, OP_GET_GLOBAL, (uint8_t)fun->slot);
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

// compiles field values in *declaration* order (matching the runtime
// instance layout), not the initializer's source order.
static void compile_struct_init(Cg *cg, Expr *expr) {
  ExprStructInit *init = &expr->as.struct_init;
  StructDef *def = init->resolved_struct->as.struc.def;

  for (int i = 0; i < def->field_count; i++) {
    FieldInit *fi = find_field_init(init->fields, init->field_count,
                                    def->is_tuple, def->fields[i].ident);
    compile_expr(cg, fi->value);
  }
  emit2(cg, OP_STRUCT, (uint8_t)def->slot);
}

static void compile_variant_init(Cg *cg, Expr *expr) {
  ExprVariant *init = &expr->as.variant;
  EnumDef *enum_def = init->resolved_enum->as.enm.def;
  VariantDef *variant = init->resolved_variant;

  for (int i = 0; i < variant->field_count; i++) {
    FieldInit *fi =
        find_field_init(init->fields, init->field_count, variant->is_tuple,
                        variant->fields[i].ident);
    compile_expr(cg, fi->value);
  }
  emit(cg, OP_ENUM);
  emit(cg, (uint8_t)enum_def->slot);
  emit(cg, (uint8_t)variant->tag);
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

static void compile_expr(Cg *cg, Expr *expr) {
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

  case EXPR_SELF:
    assert(cg->self_slot >= 0 &&
           "'self' outside a method got past the checker");
    emit2(cg, OP_GET_LOCAL, (uint8_t)cg->self_slot);
    break;

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
      FunDef *fun = expr->as.path_expr.resolved_fun;
      if (fun == NULL) {
        cg_error(cg, expr->span, "this path expression");
        emit(cg, OP_UNIT);
        break;
      }
      emit2(cg, OP_GET_GLOBAL, (uint8_t)fun->slot);
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

    FunDef *fun = cg_find_module_fun(cg, name);
    if (fun != NULL) {
      emit2(cg, OP_GET_GLOBAL, (uint8_t)fun->slot);
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

// shared by top-level functions and impl methods: `fun` is the resolved
// target (its `chunk` gets filled in), `body` and `span` come from whichever
// Decl actually holds the AST (the top-level DeclFun, or an impl item's).
static void compile_fun_body(Module *m, Heap *heap, FunDef *fun, Expr *body,
                             Span span, DiagBag *diags, Allocator *al,
                             bool *ok) {
  if (fun->type_param_count > 0) {
    // generic functions/methods need monomorphisation or boxed generics;
    // neither exists yet
    diag_error(diags, span,
               "generic functions are not supported by the VM yet");
    *ok = false;
    return;
  }

  Cg cg = {.m = m,
           .heap = heap,
           .diags = diags,
           .al = al,
           .ok = true,
           .self_slot = -1};
  cg.chunk = al_alloc_zero_for(al, Chunk);
  chunk_init(cg.chunk, al);

  for (int i = 0; i < fun->param_count; i++) {
    int slot = cg_add_local(&cg, fun->params[i].name, span);
    if (fun->params[i].is_self) {
      cg.self_slot = slot;
    }
  }

  compile_expr(&cg, body);
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

  Cg child = {.m = cg->m,
              .heap = cg->heap,
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

  int const_idx = chunk_add_const(cg->chunk, val_fun(fun));
  emit2(cg, OP_CLOSURE, (uint8_t)const_idx);
  emit(cg, (uint8_t)child.upvalue_count);
  for (int i = 0; i < child.upvalue_count; i++) {
    emit2(cg, child.upvalues[i].is_local ? 1 : 0, child.upvalues[i].index);
  }
}

static void compile_fun(Module *m, Heap *heap, Decl *decl, DiagBag *diags,
                        Allocator *al, bool *ok) {
  DeclFun *fun_decl = &decl->as.fun_decl;
  compile_fun_body(m, heap, fun_decl->def, fun_decl->body, decl->span, diags,
                   al, ok);
}

// impl methods aren't linked back to a FunDef the way top-level DeclFuns are
// (`item->fun_decl->as.fun_decl.def` is never set — resolve_impl_decl builds
// the FunDef straight into `ImplDef.methods[]` instead) — so the target
// FunDef is threaded through explicitly, paired up by iterating in the same
// order resolve_impl_decl used to fill `impl_def->methods[]` (method items,
// skipping assoc-type items).
static void compile_impl(Module *m, Heap *heap, Decl *decl, DiagBag *diags,
                         Allocator *al, bool *ok) {
  DeclImpl *impl_decl = &decl->as.impl_decl;
  ImplDef *impl_def = impl_decl->def;

  if (impl_def->type_param_count > 0) {
    // a generic impl's methods reference the impl's own type params (in
    // Self/param/return types); monomorphisation doesn't exist yet, same
    // limitation as generic functions.
    diag_error(diags, decl->span,
               "methods of a generic impl are not supported by the VM yet");
    *ok = false;
    return;
  }

  for (int i = 0, method_idx = 0; i < impl_decl->item_count; i++) {
    ImplItemNode *item = &impl_decl->items[i];
    if (item->kind != IMPL_ITEM_METHOD) {
      continue;
    }
    FunDef *fun = impl_def->methods[method_idx++].fun;
    DeclFun *fun_decl = &item->fun_decl->as.fun_decl;
    compile_fun_body(m, heap, fun, fun_decl->body, item->span, diags, al, ok);
  }
}

bool codegen_module(Module *m, Heap *heap, DiagBag *diags, Allocator *al) {
  for (int i = 0; i < m->fun_count; i++) {
    m->funs[i]->slot = i;
  }
  for (int i = 0; i < m->struct_count; i++) {
    m->structs[i]->slot = i;
  }
  for (int i = 0; i < m->enum_count; i++) {
    m->enums[i]->slot = i;
    for (int j = 0; j < m->enums[i]->variant_count; j++) {
      m->enums[i]->variants[j].tag = (uint8_t)j;
    }
  }

  // impl methods continue the funs' slot space so OP_GET_GLOBAL can address
  // either with one operand byte (see vm.c).
  int method_count = 0;
  for (int i = 0; i < m->impl_count; i++) {
    method_count += m->impls[i]->method_count;
  }
  m->methods = al_alloc_zero(al, sizeof(FunDef *) * (size_t)method_count);
  m->method_count = method_count;
  int slot = m->fun_count, method_slot = 0;
  for (int i = 0; i < m->impl_count; i++) {
    ImplDef *impl = m->impls[i];
    for (int j = 0; j < impl->method_count; j++) {
      FunDef *fun = impl->methods[j].fun;
      fun->slot = slot++;
      m->methods[method_slot++] = fun;
    }
  }

  bool ok = true;
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind == DECL_FUN) {
      compile_fun(m, heap, decl, diags, al, &ok);
    } else if (decl->kind == DECL_IMPL) {
      compile_impl(m, heap, decl, diags, al, &ok);
    }
    // structs/enums/traits: nothing executable of their own
  }
  return ok;
}
