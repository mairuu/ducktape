#pragma once

#include <stdint.h>

#include "allocator.h"
#include "array.h"
#include "string_utils.h"

typedef enum {
  RAW_NUMBER,
  RAW_STRING,
} RawType;

typedef struct RawConstant {
  RawType type;
  union {
    double number;
    String string;
  } as;
} RawConstant;

void raw_constant_destroy(RawConstant *constant, Allocator *al);

DEFINE_ARRAY(RawConstant, RawConstantArray);

typedef struct Proto {
  IntArray *lines;
  ByteArray *code;
  RawConstantArray *constants;
} Proto;

void proto_init(Proto *proto, Allocator *al);

void proto_destroy(Proto *proto, Allocator *al);

void proto_write_code(Proto *proto, uint8_t byte, int line, Allocator *al);

int proto_write_constant(Proto *proto, RawConstant constant, Allocator *al);