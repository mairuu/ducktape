#pragma once

#include "allocator.h"
#include "value.h"

#include <stdint.h>

// bytecode operations. operands are listed after each op.
typedef enum {
  OP_CONST, // u8 constant index
  OP_UNIT,
  OP_TRUE,
  OP_FALSE,
  OP_POP,
  OP_POPN,  // u8 count — pop n values
  OP_SLIDE, // u8 count — remove n values beneath the top value

  OP_GET_LOCAL,  // u8 frame slot
  OP_SET_LOCAL,  // u8 frame slot (value stays on the stack)
  OP_GET_GLOBAL, // u8 module function slot

  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_NEG,
  OP_NOT,

  OP_EQ,
  OP_NEQ,
  OP_LT,
  OP_LTEQ,
  OP_GT,
  OP_GTEQ,

  OP_CAST_INT,   // Float -> Int (truncating); Int is a no-op
  OP_CAST_FLOAT, // Int -> Float; Float is a no-op

  OP_RANGE,       // u8 inclusive — pops end, start; pushes range
  OP_RANGE_START, // pops range; pushes start
  OP_RANGE_TEST,  // pops i, range; pushes i < end (or <= when inclusive)

  OP_JUMP,          // u16 forward offset
  OP_JUMP_IF_FALSE, // u16 forward offset (does not pop the condition)
  OP_LOOP,          // u16 backward offset

  OP_CALL,  // u8 argc — callee value sits beneath the args
  OP_PRINT, // pops a value, prints it, pushes unit
  OP_RETURN,
} OpCode;

typedef struct Chunk {
  uint8_t *code;
  int count;
  int cap;

  Value *consts;
  int const_count;
  int const_cap;

  Allocator *al;
} Chunk;

void chunk_init(Chunk *c, Allocator *al);
void chunk_write(Chunk *c, uint8_t byte);
// returns the constant's index; aborts past 256 constants (u8 operand).
int chunk_add_const(Chunk *c, Value v);
