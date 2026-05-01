#include "proto.h"

#include <assert.h>

#include "array.h"
#include "string_utils.h"

void raw_constant_destroy(RawConstant *constant, Allocator *al) {
  switch (constant->type) {
  case RAW_NUMBER:
    break;
  case RAW_STRING:
    str_destroy(&constant->as.string, al);
    break;
  case RAW_FUNCTION:
    proto_destroy(constant->as.function, al);
    al_free_for(al, constant->as.function);
    break;
  default:
    assert(false && "unknown raw constant type");
  }
}

void proto_init(Proto *proto, Allocator *al) {
  (void)al;
  proto->name = str_null();
  proto->arity = 0;
  proto->upvalue_count = 0;
  proto->lines = arr_new();
  proto->code = arr_new();
  proto->constants = arr_new();
  proto->patches = arr_new();
}

void proto_destroy(Proto *proto, Allocator *al) {
  int constant_count = arr_count(proto->constants);
  for (int i = 0; i < constant_count; i++) {
    raw_constant_destroy(&proto->constants->data[i], al);
  }

  str_destroy(&proto->name, al);

  arr_free(&proto->lines, al);
  arr_free(&proto->code, al);
  arr_free(&proto->constants, al);
  arr_free(&proto->patches, al);
}

void proto_write_code(Proto *proto, uint8_t byte, int line, Allocator *al) {
  arr_push(proto->code, byte, al);
  arr_push(proto->lines, line, al);
}

int proto_write_constant(Proto *proto, RawConstant constant, Allocator *al) {
  arr_push(proto->constants, constant, al);
  return arr_count(proto->constants) - 1;
}

void proto_add_patch(Proto *proto, int offset, Allocator *al) {
  arr_push(proto->patches, offset, al);
}

void proto_patch_short(Proto *proto, int offset, uint16_t value) {
  proto->code->data[offset] = (value >> 8) & 0xff; // high byte.
  proto->code->data[offset + 1] = value & 0xff;    // low byte.
}