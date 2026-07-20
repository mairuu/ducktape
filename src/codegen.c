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

typedef struct {
  StringView name; // empty for hidden locals
} CgLocal;

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

typedef struct {
  Module *m;
  Heap *heap;
  Chunk *chunk;
  DiagBag *diags;
  Allocator *al;

  CgLocal locals[CG_MAX_LOCALS];
  int local_count;

  CgLoop *loop;
  bool ok;
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

static FunDef *cg_find_module_fun(Cg *cg, StringView name) {
  for (int i = 0; i < cg->m->fun_count; i++) {
    if (sv_equal(cg->m->funs[i]->name, name)) {
      return cg->m->funs[i];
    }
  }
  return NULL;
}

// ── expression / statement compilation ───────────────────────────────────────

static void compile_expr(Cg *cg, Expr *expr);

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
  int slot = cg_find_local(cg, name);
  if (slot < 0) {
    cg_error(cg, assign->target->span, "assignment to a non-local");
    emit(cg, OP_UNIT);
    return;
  }

  if (assign->op == TOKEN_EQ) {
    compile_expr(cg, assign->value);
  } else {
    emit2(cg, OP_GET_LOCAL, (uint8_t)slot);
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

  emit2(cg, OP_SET_LOCAL, (uint8_t)slot); // value stays as the expr result
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

  case EXPR_PATH: {
    Path *path = &expr->as.path_expr.path;
    if (path->count != 1) {
      cg_error(cg, expr->span, "this path expression");
      emit(cg, OP_UNIT);
      break;
    }
    StringView name = path->segments[0].name;

    int slot = cg_find_local(cg, name);
    if (slot >= 0) {
      emit2(cg, OP_GET_LOCAL, (uint8_t)slot);
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

static void compile_fun(Module *m, Heap *heap, Decl *decl, DiagBag *diags,
                        Allocator *al, bool *ok) {
  DeclFun *fun_decl = &decl->as.fun_decl;
  FunDef *fun = fun_decl->def;

  if (fun->type_param_count > 0) {
    // generic functions need monomorphisation or boxed generics; neither
    // exists yet
    diag_error(diags, decl->span,
               "generic functions are not supported by the VM yet");
    *ok = false;
    return;
  }

  Cg cg = {.m = m, .heap = heap, .diags = diags, .al = al, .ok = true};
  cg.chunk = al_alloc_zero_for(al, Chunk);
  chunk_init(cg.chunk, al);

  for (int i = 0; i < fun->param_count; i++) {
    cg_add_local(&cg, fun->params[i].name, decl->span);
  }

  compile_expr(&cg, fun_decl->body);
  emit(&cg, OP_RETURN);

  fun->chunk = cg.chunk;
  *ok &= cg.ok;
}

bool codegen_module(Module *m, Heap *heap, DiagBag *diags, Allocator *al) {
  for (int i = 0; i < m->fun_count; i++) {
    m->funs[i]->slot = i;
  }

  bool ok = true;
  for (int i = 0; i < m->ast->decl_count; i++) {
    Decl *decl = m->ast->decls[i];
    if (decl->kind == DECL_FUN) {
      compile_fun(m, heap, decl, diags, al, &ok);
    }
    // impls/structs/enums/traits: nothing executable yet
  }
  return ok;
}
