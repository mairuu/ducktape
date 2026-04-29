#include "chunk.h"
#include "array.h"

void chunk_init(Chunk *chunk) {
  chunk->code = arr_new();
  chunk->lines = arr_new();
  chunk->constants = arr_new();
}

void chunk_destroy(Chunk *chunk, Allocator *al) {
  arr_free(&chunk->code, al);
  arr_free(&chunk->lines, al);
  arr_free(&chunk->constants, al);
}

void chunk_load_from_proto(Chunk *chunk, const Proto *proto, ConstantLoader *loader,
                           Allocator *al) {
  chunk_destroy(chunk, al);
  chunk_init(chunk);

  chunk->code = arr_copy(proto->code, al);
  chunk->lines = arr_copy(proto->lines, al);

  int constant_count = arr_count(proto->constants);
  arr_reserve(&chunk->constants, constant_count, al);
  for (int i = 0; i < constant_count; i++) {
    const RawConstant *raw_constant = &proto->constants->data[i];
    Value constant = loader->load_constant(loader, al, raw_constant);
    arr_push(chunk->constants, constant, al);
  }
}