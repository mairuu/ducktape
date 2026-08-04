#include "allocator.h"
#include "arena.h"
#include "bytecode.h"
#include "compiler.h"

#include <stdio.h>
#include <string.h>

static Compiler compiler;

// run a bytecode image: no source, no compiler state, just an arena to hold
// the decoded program for as long as the VM needs it.
static bool run_image(const char *path, bool gc_stress, Allocator *heap_al) {
  Arena arena;
  arena_init(&arena, heap_al);
  Allocator al = arena_allocator_create(&arena);

  bool ok = bytecode_execute(path, gc_stress, &al);

  arena_destroy(&arena);
  return ok;
}

int main(int argc, char *argv[]) {
  bool run = false;
  bool gc_stress = false;
  const char *root_path = NULL;
  const char *emit_path = NULL;
  const char *std_module = NULL;
  bool bad_args = false;
  LintLevels lints;
  lint_levels_init(&lints);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--run") == 0) {
      run = true;
    } else if (strcmp(argv[i], "--gc-stress") == 0) {
      gc_stress = true;
    } else if (strcmp(argv[i], "--emit-bc") == 0) {
      if (++i == argc) {
        bad_args = true;
        break;
      }
      emit_path = argv[i];
    } else if (strcmp(argv[i], "--std-module") == 0) {
      // lint one standard library module *as* that module: the tree comes from
      // the declarations, so the module path says which one it is and nothing
      // is inferred from the shape of a filesystem path.
      if (++i == argc) {
        bad_args = true;
        break;
      }
      std_module = argv[i];
    } else if (argv[i][0] == '-' && argv[i][1] == 'W') {
      // parsed here rather than after compiler_init so a bad lint name is
      // caught whatever else the command line turns out to mean.
      if (!lint_levels_flag(&lints, argv[i])) {
        fprintf(stderr,
                "error: '%s' names no lint\n       available: %s\n"
                "       spellings: -Werror, -Werror=<lint>, -Wno-<lint>, "
                "-W<lint>\n",
                argv[i], diag_lint_names());
        return 1;
      }
    } else if (root_path == NULL) {
      root_path = argv[i];
    } else {
      bad_args = true; // more than one file
      break;
    }
  }

  if (bad_args || (root_path == NULL) == (std_module == NULL)) {
    fprintf(stderr,
            "usage: %s [--run] [--gc-stress] [--emit-bc <out>] [-W…] <file>\n"
            "       %s --std-module std::<module> [-W…]\n"
            "       -W flags: -Werror, -Werror=<lint>, -Wno-<lint>, -W<lint>\n"
            "       lints: %s\n",
            argv[0], argv[0], diag_lint_names());
    return 1;
  }

  Allocator heap_al = heap_allocator_create();

  // `--run` accepts either form, told apart by the image magic rather than by
  // an extension or a flag: running a program should not depend on how it was
  // spelled on disk.
  if (root_path != NULL && bc_is_image(root_path)) {
    if (!run || emit_path != NULL) {
      fprintf(stderr, "error: '%s' is a bytecode image; only --run applies\n",
              root_path);
      return 1;
    }
    return run_image(root_path, gc_stress, &heap_al) ? 0 : 1;
  }

  compiler_init(&compiler, &heap_al);
  compiler_set_lints(&compiler, &lints);
  bool ok = compiler_run(&compiler, root_path, std_module);
  if (ok && emit_path != NULL) {
    ok = compiler_emit(&compiler, emit_path);
  }
  if (ok && run) {
    ok = compiler_execute(&compiler, gc_stress);
  }
  compiler_destroy(&compiler, &heap_al);

  return ok ? 0 : 1;
}
