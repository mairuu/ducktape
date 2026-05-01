#pragma once

#include "array.h"
#include "proto.h"
#include "value.h"

DEFINE_ARRAY(Value, ValueArray);

typedef struct ConstantLoader ConstantLoader;

typedef Value (*LoadConstantFunc)(ConstantLoader *loader, Allocator *al,
                                  const RawConstant *raw_constant);

typedef struct ConstantLoader {
  LoadConstantFunc load_constant;
  void *ctx;
} ConstantLoader;

typedef struct {
  ByteArray *code;
  IntArray *lines;
  ValueArray *constants;
} Chunk;

void chunk_init(Chunk *chunk);

void chunk_destroy(Chunk *chunk, Allocator *al);

void chunk_load_from_proto(Chunk *chunk, const Proto *proto, ConstantLoader *loader, Allocator *al);

void chunk_patch_byte(Chunk *chunk, int offset, uint8_t byte);