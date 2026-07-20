#include "allocator.h"
#include "compiler.h"

#include <stdio.h>
#include <string.h>

static Compiler compiler;

int main(int argc, char *argv[]) {
  bool run = false;
  bool gc_stress = false;
  const char *root_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--run") == 0) {
      run = true;
    } else if (strcmp(argv[i], "--gc-stress") == 0) {
      gc_stress = true;
    } else if (root_path == NULL) {
      root_path = argv[i];
    } else {
      root_path = NULL; // more than one file
      break;
    }
  }

  if (root_path == NULL) {
    fprintf(stderr, "usage: %s [--run] [--gc-stress] <file>\n", argv[0]);
    return 1;
  }

  Allocator heap_al = heap_allocator_create();

  compiler_init(&compiler, &heap_al);
  bool ok = compiler_run(&compiler, root_path);
  if (ok && run) {
    ok = compiler_execute(&compiler, gc_stress);
  }
  compiler_destroy(&compiler, &heap_al);

  return ok ? 0 : 1;
}