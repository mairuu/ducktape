#pragma once

#include <stdint.h>

#include "allocator.h"
#include "array.h"
#include "string_utils.h"

typedef enum {
  RAW_NUMBER,
  RAW_STRING,
  RAW_FUNCTION,
} RawType;

typedef struct Proto Proto;

typedef struct RawConstant {
  RawType type;
  union {
    double number;
    String string;
    Proto *function;
  } as;
} RawConstant;

void raw_constant_destroy(RawConstant *constant, Allocator *al);

DEFINE_ARRAY(RawConstant, RawConstantArray);

typedef struct Proto {
  String name;
  int arity;
  int upvalue_count;
  IntArray *lines;
  ByteArray *code;
  RawConstantArray *constants;
  IntArray *pics;         // polymorphic inline cache
  IntArray *patches;     // patch sites for definitions
  IntArray *param_constraints; // length == arity, for storing type constraints of function parameters
} Proto;

void proto_init(Proto *proto, Allocator *al);

void proto_destroy(Proto *proto, Allocator *al);

void proto_write_code(Proto *proto, uint8_t byte, int line, Allocator *al);

int proto_write_constant(Proto *proto, RawConstant constant, Allocator *al);

int proto_register_pic(Proto *proto, int constant_index, Allocator *al);

void proto_add_patch(Proto *proto, int offset,
                     Allocator *al); // mark a patch site that needs to be patched later.

void proto_patch_short(Proto *proto, int offset,
                       uint16_t value); // patch a short (2 bytes) value at the given offset.

void proto_set_param_constraint(Proto *proto, int param_index, int def_index, Allocator *al); // set the definition index of the constraint for the parameter at the given index.