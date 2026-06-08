#include "allocator.h"
#include "compiler.h"

#include <stdio.h>

static Compiler compiler;

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file>\n", argv[0]);
    return 1;
  }

  const char *root_path = argv[1];

  Allocator heap_al = heap_allocator_create();

  compiler_init(&compiler, &heap_al);
  compiler_run(&compiler, root_path);
  compiler_destroy(&compiler, &heap_al);

  return 0;
}