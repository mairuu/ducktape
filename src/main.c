#include "allocator.h"
#include "virtual_machine.h"
#include <stdio.h>
#include <stdlib.h>

static VirtualMachine vm;

static void repl(VirtualMachine *vm);
static int run_file(VirtualMachine *vm, const char *path);

int main(int argc, char *argv[]) {
  Allocator heap_al = heap_allocator_create();

  TrackAllocator ta;
  track_alloc_init(&ta, &heap_al);
  Allocator al = track_allocator_create(&ta);

  vm_init(&vm, al);

  if (argc == 1) {
    repl(&vm);
  } else if (argc == 2) {
    run_file(&vm, argv[1]);
  } else {
    fprintf(stderr, "usage: interpreter [path]\n");
  }

  vm_destroy(&vm);

  track_alloc_report(&ta);
  track_alloc_destroy(&ta);

  return 0;
}

static void repl(VirtualMachine *vm) {
  char line[1024];

  while (true) {
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    InterpretResult result = vm_interpret(vm, line);
    if (result == INTERPRET_COMPILE_ERROR) {
      fprintf(stderr, "compile error.\n");
    } else if (result == INTERPRET_RUNTIME_ERROR) {
      fprintf(stderr, "runtime error.\n");
    }
  }
}

static int run_file(VirtualMachine *vm, const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "could not open file '%s'.\n", path);
    return -1;
  }

  fseek(file, 0L, SEEK_END);
  size_t file_size = ftell(file);
  rewind(file);

  char *buffer = (char *)malloc(file_size + 1);
  if (buffer == NULL) {
    fprintf(stderr, "not enough memory to read '%s'.\n", path);
    return -1;
  }

  size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
  if (bytes_read < file_size) {
    fprintf(stderr, "could not read file '%s'.\n", path);
    return -1;
  }

  buffer[bytes_read] = '\0';
  InterpretResult result = vm_interpret(vm, buffer);

  if (result == INTERPRET_COMPILE_ERROR) {
    fprintf(stderr, "compile error.\n");
    return -1;
  } else if (result == INTERPRET_RUNTIME_ERROR) {
    fprintf(stderr, "runtime error.\n");
    return -1;
  }

  free(buffer);
  return 0;
}