#pragma once

#include "allocator.h"
#include "chunk.h"

#include <stdint.h>

// The instruction list one body is built into, and the walk that turns it into
// a `Chunk`. Codegen appends here instead of writing bytes, which buys two
// things it used to do by hand: a forward jump names a *label* rather than a
// byte offset patched in place, and the rewrites that unemit look at the last
// instruction instead of decoding the bytes behind them.
//
// It is not a value IR. An instruction is still one the VM executes, carrying
// the same operands, and `ir_emit` is a straight walk — what is new is that the
// walk happens once the body is complete, so a position in it is an index and
// not an address.

// An operand shape. This is chunk.h's taxonomy: one byte when the operand
// indexes a frame, two when it indexes a table the program grows.
typedef enum {
  IR_NONE,    // no operand
  IR_U8,      // frame slot, count, field index, variant tag
  IR_U16,     // constant, global, struct, enum or vtable slot
  IR_U16_U8,  // OP_ENUM: the enum's slot, then the variant tag
  IR_LABEL,   // OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP
  IR_CLOSURE, // OP_CLOSURE: a constant, then the upvalue pairs
} IrShape;

typedef struct {
  uint8_t op;
  uint8_t shape;
  int a; // the operand, or the label a jump names
  int b; // OP_ENUM's variant tag
  // OP_CLOSURE's (is_local, index) pairs, copied into the arena.
  const uint8_t *pairs;
  int pair_count;
} IrInst;

#define IR_UNBOUND (-1)

typedef struct {
  IrInst *insts;
  int count;
  int cap;

  // label → the instruction index it names, `IR_UNBOUND` until bound. A label
  // bound at `count` names the end of the body, which is where a forward jump
  // out of the construct being closed lands.
  int *labels;
  int label_count;
  int label_cap;

  Allocator *al;
} IrBody;

void ir_init(IrBody *ir, Allocator *al);

void ir_op(IrBody *ir, uint8_t op);
void ir_op_u8(IrBody *ir, uint8_t op, int v);
void ir_op_u16(IrBody *ir, uint8_t op, int v);
void ir_op_u16_u8(IrBody *ir, uint8_t op, int a, int b);
void ir_closure(IrBody *ir, int const_idx, const uint8_t *pairs,
                int pair_count);

// a label naming nowhere yet.
int ir_label(IrBody *ir);
// bind it to the position after the last instruction emitted.
void ir_bind(IrBody *ir, int label);
// the instruction index a bound label names.
int ir_label_at(const IrBody *ir, int label);
void ir_jump(IrBody *ir, uint8_t op, int label);

// the position after the last instruction: where a label bound now would land.
static inline int ir_count(const IrBody *ir) { return ir->count; }
const IrInst *ir_last(const IrBody *ir);
// Drop instructions from the end — the peephole's unemit. Nothing bound may be
// left naming code that no longer exists, and nothing is: every caller
// unemits what it emitted since the last bind.
void ir_truncate(IrBody *ir, int count);

// walk the list into `c`, resolving each label to the distance from the end of
// the jump that names it.
void ir_emit(const IrBody *ir, Chunk *c);
