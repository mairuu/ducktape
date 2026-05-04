#pragma once

typedef enum {
  OP_CONSTANT,
  OP_CLOSURE,
  OP_BYTE, // push a single byte as a number (for small integers)

  OP_POP,
  OP_DUP,

  OP_ADD,
  OP_SUBTRACT,
  OP_MULTIPLY,
  OP_DIVIDE,
  OP_NEGATE,

  OP_NOT,
  OP_EQUAL,
  OP_GREATER,
  OP_LESS,

  OP_NIL,
  OP_TRUE,
  OP_FALSE,

  OP_DEFINE_GLOBAL,
  OP_GET_GLOBAL,
  OP_SET_GLOBAL,

  OP_GET_LOCAL,
  OP_SET_LOCAL,

  OP_GET_UPVALUE,
  OP_SET_UPVALUE,
  OP_CLOSE_UPVALUE,

  OP_GET_DEFINITION,

  OP_GET_PROPERTY,
  OP_SET_PROPERTY,

  OP_LOOP,
  OP_JUMP,
  OP_JUMP_IF_FALSE,

  OP_CALL,

  // stack: [trait, function] -> [trait]
  // operand: index of the default method in the trait's definition
  OP_IMPL_DEFAULT,

  OP_TRAP,
  OP_RETURN,
} OpCode;