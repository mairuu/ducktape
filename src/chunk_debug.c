#include "chunk_debug.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "array.h"
#include "object.h"
#include "opcode.h"

void disassemble_chunk(Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);

  int count = arr_count(chunk->code);
  for (int offset = 0; offset < count;) {
    offset = disassemble_chunk_instruction(chunk, offset);
  }
}

static int simple_instruction(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

static int constant_instruction(const char *name, Chunk *chunk, int offset) {
  uint8_t constant_index = chunk->code->data[offset + 1];
  printf("%-16s %d '", name, constant_index);
  Value constant = chunk->constants->data[constant_index];
  val_print(constant);
  printf("'\n");
  return offset + 2;
}

static int pic_instruction(const char *name, Chunk *chunk, int offset) {
  uint8_t pic_index = chunk->code->data[offset + 1];
  uint8_t constant_index = chunk->pic->data[pic_index];
  printf("%-16s %d '", name, constant_index);
  Value constant = chunk->constants->data[constant_index];
  val_print(constant);
  printf("'\n");
  return offset + 2;
}

static int closure_instruction(const char *name, Chunk *chunk, int offset) {
  offset++;
  uint8_t constant_index = chunk->code->data[offset++];
  printf("%-16s %d '", name, constant_index);
  val_print(chunk->constants->data[constant_index]);
  printf("'\n");

  ObjFunction *function = AS_FUNCTION(chunk->constants->data[constant_index]);
  for (int j = 0; j < function->upvalue_count; j++) {
    int is_local = chunk->code->data[offset++];
    int index = chunk->code->data[offset++];
    printf("%04d      |                   %s %d\n", offset - 2, is_local ? "local" : "upvalue",
           index);
  }

  return offset;
}

static int byte_instruction(const char *name, Chunk *chunk, int offset) {
  uint8_t slot = chunk->code->data[offset + 1];
  printf("%-16s %d\n", name, slot);
  return offset + 2;
}

static int jump_instruction(const char *name, Chunk *chunk, bool is_forward, int offset) {
  uint16_t jump = (uint16_t)(chunk->code->data[offset + 1] << 8);
  jump |= chunk->code->data[offset + 2];
  printf("%-16s %04d -> %04d\n", name, offset, offset + 3 + (is_forward ? jump : -jump));
  return offset + 3;
}

int disassemble_chunk_instruction(Chunk *chunk, int offset) {
  printf("%04d ", offset);
  if (offset > 0 && chunk->lines->data[offset] == chunk->lines->data[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines->data[offset]);
  }

  uint8_t instruction = chunk->code->data[offset];

  switch (instruction) {
  case OP_CONSTANT:
    return constant_instruction("OP_CONSTANT", chunk, offset);
  case OP_CLOSURE:
    return closure_instruction("OP_CLOSURE", chunk, offset);
  case OP_BYTE:
    return byte_instruction("OP_BYTE", chunk, offset);

  case OP_POP:
    return simple_instruction("OP_POP", offset);
  case OP_DUP:
    return simple_instruction("OP_DUP", offset);

  case OP_ADD:
    return simple_instruction("OP_ADD", offset);
  case OP_SUBTRACT:
    return simple_instruction("OP_SUBTRACT", offset);
  case OP_MULTIPLY:
    return simple_instruction("OP_MULTIPLY", offset);
  case OP_DIVIDE:
    return simple_instruction("OP_DIVIDE", offset);
  case OP_NEGATE:
    return simple_instruction("OP_NEGATE", offset);

  case OP_NOT:
    return simple_instruction("OP_NOT", offset);
  case OP_EQUAL:
    return simple_instruction("OP_EQUAL", offset);
  case OP_GREATER:
    return simple_instruction("OP_GREATER", offset);
  case OP_LESS:
    return simple_instruction("OP_LESS", offset);

  case OP_NIL:
    return simple_instruction("OP_NIL", offset);
  case OP_TRUE:
    return simple_instruction("OP_TRUE", offset);
  case OP_FALSE:
    return simple_instruction("OP_FALSE", offset);

  case OP_DEFINE_GLOBAL:
    return constant_instruction("OP_DEFINE_GLOBAL", chunk, offset);
  case OP_GET_GLOBAL:
    return constant_instruction("OP_GET_GLOBAL", chunk, offset);
  case OP_SET_GLOBAL:
    return constant_instruction("OP_SET_GLOBAL", chunk, offset);

  case OP_GET_LOCAL:
    return byte_instruction("OP_GET_LOCAL", chunk, offset);
  case OP_SET_LOCAL:
    return byte_instruction("OP_SET_LOCAL", chunk, offset);

  case OP_GET_UPVALUE:
    return byte_instruction("OP_GET_UPVALUE", chunk, offset);
  case OP_SET_UPVALUE:
    return byte_instruction("OP_SET_UPVALUE", chunk, offset);
  case OP_CLOSE_UPVALUE:
    return simple_instruction("OP_CLOSE_UPVALUE", offset);

  case OP_GET_DEFINITION:
    return byte_instruction("OP_DEFINITION", chunk, offset);

  case OP_GET_PROPERTY:
    return pic_instruction("OP_GET_PROPERTY", chunk, offset);
  case OP_SET_PROPERTY:
    return pic_instruction("OP_SET_PROPERTY", chunk, offset);

  case OP_LOOP:
    return jump_instruction("OP_LOOP", chunk, false, offset);
  case OP_JUMP:
    return jump_instruction("OP_JUMP", chunk, true, offset);
  case OP_JUMP_IF_FALSE:
    return jump_instruction("OP_JUMP_IF_FALSE", chunk, true, offset);

  case OP_CALL:
    return byte_instruction("OP_CALL", chunk, offset);
  
  case OP_IMPL:
    return simple_instruction("OP_IMPL", offset);
  case OP_IMPL_METHOD:
    return byte_instruction("OP_IMPL_METHOD", chunk, offset);
  
  case OP_CAST:
    return simple_instruction("OP_CAST", offset);

  case OP_TRAP:
    return simple_instruction("OP_TRAP", offset);
  case OP_RETURN:
    return simple_instruction("OP_RETURN", offset);
  default:
    assert(false && "unknown opcode");
    printf("unknown opcode %d\n", instruction);
    return offset + 1;
  }
}