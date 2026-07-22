#include "chunk.h"

#include <assert.h>

void chunk_init(Chunk *c, Allocator *al) {
  *c = (Chunk){0};
  c->al = al;
}

void chunk_write(Chunk *c, uint8_t byte) {
  if (c->count >= c->cap) {
    int new_cap = c->cap == 0 ? 32 : c->cap * 2;
    c->code = al_realloc(c->al, c->code, c->cap, new_cap);
    assert(c->code && "out of memory");
    c->cap = new_cap;
  }
  c->code[c->count++] = byte;
}

void chunk_write_u16(Chunk *c, uint16_t v) {
  chunk_write(c, (uint8_t)((v >> 8) & 0xff));
  chunk_write(c, (uint8_t)(v & 0xff));
}

int chunk_add_const(Chunk *c, Value v) {
  if (c->const_count == SLOT_MAX) {
    return -1;
  }
  if (c->const_count >= c->const_cap) {
    int new_cap = c->const_cap == 0 ? 8 : c->const_cap * 2;
    c->consts = al_realloc(c->al, c->consts, sizeof(Value) * c->const_cap,
                           sizeof(Value) * new_cap);
    assert(c->consts && "out of memory");
    c->const_cap = new_cap;
  }
  c->consts[c->const_count] = v;
  return c->const_count++;
}
