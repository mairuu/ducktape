#include "ir.h"

#include <assert.h>
#include <string.h>

void ir_init(IrBody *ir, Allocator *al) {
  *ir = (IrBody){0};
  ir->al = al;
}

static IrInst *ir_push(IrBody *ir) {
  if (ir->count >= ir->cap) {
    int new_cap = ir->cap == 0 ? 32 : ir->cap * 2;
    ir->insts = al_realloc(ir->al, ir->insts, sizeof(IrInst) * (size_t)ir->cap,
                           sizeof(IrInst) * (size_t)new_cap);
    assert(ir->insts && "out of memory");
    ir->cap = new_cap;
  }
  IrInst *inst = &ir->insts[ir->count++];
  *inst = (IrInst){0};
  return inst;
}

void ir_op(IrBody *ir, uint8_t op) {
  IrInst *inst = ir_push(ir);
  inst->op = op;
  inst->shape = IR_NONE;
}

void ir_op_u8(IrBody *ir, uint8_t op, int v) {
  assert(v >= 0 && v <= 0xff && "operand outside the byte it is written into");
  IrInst *inst = ir_push(ir);
  inst->op = op;
  inst->shape = IR_U8;
  inst->a = v;
}

void ir_op_u16(IrBody *ir, uint8_t op, int v) {
  assert(v >= 0 && v < SLOT_MAX && "slot outside the operand space");
  IrInst *inst = ir_push(ir);
  inst->op = op;
  inst->shape = IR_U16;
  inst->a = v;
}

void ir_op_u16_u8(IrBody *ir, uint8_t op, int a, int b) {
  assert(a >= 0 && a < SLOT_MAX && "slot outside the operand space");
  assert(b >= 0 && b <= 0xff && "operand outside the byte it is written into");
  IrInst *inst = ir_push(ir);
  inst->op = op;
  inst->shape = IR_U16_U8;
  inst->a = a;
  inst->b = b;
}

void ir_closure(IrBody *ir, int const_idx, const uint8_t *pairs,
                int pair_count) {
  assert(const_idx >= 0 && const_idx < SLOT_MAX && "constant outside the pool");
  assert(pair_count >= 0 && pair_count <= 0xff && "too many upvalues");
  uint8_t *owned = al_alloc(ir->al, sizeof(uint8_t) * 2 * (size_t)pair_count);
  memcpy(owned, pairs, sizeof(uint8_t) * 2 * (size_t)pair_count);
  IrInst *inst = ir_push(ir);
  inst->op = OP_CLOSURE;
  inst->shape = IR_CLOSURE;
  inst->a = const_idx;
  inst->pairs = owned;
  inst->pair_count = pair_count;
}

int ir_label(IrBody *ir) {
  if (ir->label_count >= ir->label_cap) {
    int new_cap = ir->label_cap == 0 ? 16 : ir->label_cap * 2;
    ir->labels =
        al_realloc(ir->al, ir->labels, sizeof(int) * (size_t)ir->label_cap,
                   sizeof(int) * (size_t)new_cap);
    assert(ir->labels && "out of memory");
    ir->label_cap = new_cap;
  }
  ir->labels[ir->label_count] = IR_UNBOUND;
  return ir->label_count++;
}

void ir_bind(IrBody *ir, int label) {
  assert(label >= 0 && label < ir->label_count && "no such label");
  ir->labels[label] = ir->count;
}

int ir_label_at(const IrBody *ir, int label) {
  assert(label >= 0 && label < ir->label_count && "no such label");
  return ir->labels[label];
}

void ir_jump(IrBody *ir, uint8_t op, int label) {
  assert(label >= 0 && label < ir->label_count && "no such label");
  IrInst *inst = ir_push(ir);
  inst->op = op;
  inst->shape = IR_LABEL;
  inst->a = label;
}

const IrInst *ir_last(const IrBody *ir) {
  return ir->count > 0 ? &ir->insts[ir->count - 1] : NULL;
}

void ir_truncate(IrBody *ir, int count) {
  assert(count >= 0 && count <= ir->count && "truncating past the end");
  ir->count = count;
}

static int inst_size(const IrInst *inst) {
  switch (inst->shape) {
  case IR_NONE:
    return 1;
  case IR_U8:
    return 2;
  case IR_U16:
  case IR_LABEL:
    return 3;
  case IR_U16_U8:
    return 4;
  case IR_CLOSURE:
    return 4 + 2 * inst->pair_count;
  }
  assert(false && "unhandled operand shape");
  return 0;
}

void ir_emit(const IrBody *ir, Chunk *c) {
  // Every instruction's width is known from its shape, so one pass fixes the
  // address of each and the second resolves the labels against them. A jump's
  // offset is measured from the end of the jump itself, in both directions.
  int *at = al_alloc(ir->al, sizeof(int) * (size_t)(ir->count + 1));
  int offset = 0;
  for (int i = 0; i < ir->count; i++) {
    at[i] = offset;
    offset += inst_size(&ir->insts[i]);
  }
  at[ir->count] = offset;

  for (int i = 0; i < ir->count; i++) {
    const IrInst *inst = &ir->insts[i];
    chunk_write(c, inst->op);
    switch (inst->shape) {
    case IR_NONE:
      break;
    case IR_U8:
      chunk_write(c, (uint8_t)inst->a);
      break;
    case IR_U16:
      chunk_write_u16(c, (uint16_t)inst->a);
      break;
    case IR_U16_U8:
      chunk_write_u16(c, (uint16_t)inst->a);
      chunk_write(c, (uint8_t)inst->b);
      break;
    case IR_LABEL: {
      int target = ir->labels[inst->a];
      assert(target != IR_UNBOUND && "a jump names a label nothing bound");
      int after = at[i] + 3;
      int dist = inst->op == OP_LOOP ? after - at[target] : at[target] - after;
      assert(dist >= 0 && dist <= 0xffff && "jump too far");
      chunk_write_u16(c, (uint16_t)dist);
      break;
    }
    case IR_CLOSURE:
      chunk_write_u16(c, (uint16_t)inst->a);
      chunk_write(c, (uint8_t)inst->pair_count);
      for (int p = 0; p < 2 * inst->pair_count; p++) {
        chunk_write(c, inst->pairs[p]);
      }
      break;
    }
  }
  al_free(ir->al, at, sizeof(int) * (size_t)(ir->count + 1));
}
