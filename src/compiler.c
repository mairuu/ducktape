#include "compiler.h"
#include "allocator.h"
#include "arena.h"
#include "ast.h"
#include "bytecode.h"
#include "codegen.h"
#include "diag.h"
#include "module.h"
#include "object.h"
#include "sema.h"
#include "string_utils.h"
#include "vm.h"

#include <assert.h>

void compiler_init(Compiler *c, Allocator *al) {
  arena_init(&c->arena, al);
  c->al = arena_allocator_create(&c->arena);

  diag_init(&c->diags, &c->al);
  modreg_init(&c->mod_reg, &c->al);
  tc_init(&c->tc, &c->diags, &c->al);
}

void compiler_destroy(Compiler *c, Allocator *al) {
  (void)al;
  arena_destroy(&c->arena);

  diag_destroy(&c->diags);
  modreg_destroy(&c->mod_reg);
  tc_destroy(&c->tc);
}

// phase 0: discover and parse every source file reachable from the root.
//
// discovery can't precede parsing — a module's dependencies are its `use`
// declarations, which only exist once it has an AST. So this is a worklist:
// parse a module, collect its imports (registering any file not seen before),
// and continue into the modules that just appeared.
static bool compiler_phase_discover(Compiler *c, const char *root_path) {
  StringView root = path_normalise(sv_from_cstr(root_path), &c->al);
  StringView base_dir = path_dir_of(root);

  Module *root_mod = mod_new(root, &c->al);
  modreg_add(&c->mod_reg, root_mod);
  c->root_module = root_mod;

  bool had_errors = false;

  // the bound is re-read each iteration: mod_collect_imports appends to the
  // registry, and those appends are what extend the loop. reg->modules is
  // realloc'd as it grows, so never cache the array across iterations.
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = c->mod_reg.modules[i];

    if (!mod_parse(m, &c->diags, &c->al)) {
      had_errors = true;
      if (diag_has_diags(&c->diags)) {
        diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
      }
      continue; // no AST, so no imports to collect
    }

    had_errors |=
        !mod_collect_imports(m, &c->mod_reg, base_dir, &c->diags, &c->al);

    // must report before the next iteration: mod_parse clears the bag.
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }
  }

  return !had_errors;
}

// report a dependency cycle, anchored at the import that closes it. one note
// per edge rather than a chain built into a fixed buffer — that accumulation
// pattern is what produced the type_sprintf overflow fixed after 6b.
static void report_cycle(Compiler *c, const int *stack, int sp, int back_to,
                         ModImport *imp) {
  // the module holding `imp` is the one on top of the path stack.
  Module *closer = c->mod_reg.modules[stack[sp - 1]];
  diag_error(&c->diags, imp->decl->as.use_decl.path.span,
             "circular module dependency");

  int start = 0;
  while (start < sp && stack[start] != back_to) {
    start++;
  }
  for (int i = start; i < sp; i++) {
    Module *from = c->mod_reg.modules[stack[i]];
    Module *to = (i + 1 < sp) ? c->mod_reg.modules[stack[i + 1]]
                              : c->mod_reg.modules[back_to];
    diag_note(&c->diags, (Span){0}, "  " SV_FMT " imports " SV_FMT,
              SV_ARG(from->file_path), SV_ARG(to->file_path));
  }

  diag_report(&c->diags, closer->file_path.chars, closer->source.chars, stderr);
}

// tri-colour DFS. appends on post-order, so dependencies land in topo_order
// before their dependents — the order every later phase assumes.
static bool topo_visit(Compiler *c, int idx, unsigned char *colour, int *stack,
                       int *sp) {
  colour[idx] = 1; // grey: on the current path
  stack[(*sp)++] = idx;

  Module *m = c->mod_reg.modules[idx];
  for (int i = 0; i < m->import_count; i++) {
    ModImport *imp = &m->imports[i];
    if (imp->is_std || imp->module_index < 0) {
      continue;
    }
    int dep = imp->module_index;

    if (colour[dep] == 1) { // back edge onto the current path
      report_cycle(c, stack, *sp, dep, imp);
      return false;
    }
    if (colour[dep] == 0 && !topo_visit(c, dep, colour, stack, sp)) {
      return false;
    }
  }

  (*sp)--;
  colour[idx] = 2; // black: finished
  c->mod_reg.topo_order[c->mod_reg.topo_count++] = idx;
  return true;
}

// phase 0: build the dependency graph and topologically sort modules.
static bool compiler_phase_dep_graph(Compiler *c) {
  int n = c->mod_reg.module_count;
  c->mod_reg.topo_order = al_alloc(&c->al, sizeof(int) * n);
  c->mod_reg.topo_count = 0;

  unsigned char *colour = al_alloc_zero(&c->al, sizeof(unsigned char) * n);
  int *stack = al_alloc(&c->al, sizeof(int) * n);
  int sp = 0;

  bool ok = true;
  for (int i = 0; i < n && ok; i++) {
    if (colour[i] == 0) {
      ok = topo_visit(c, i, colour, stack, &sp);
    }
  }

  al_free(&c->al, colour, sizeof(unsigned char) * n);
  al_free(&c->al, stack, sizeof(int) * n);

  if (!ok) {
    return false; // stop at the first cycle; a second pass is just noise
  }

  assert(c->mod_reg.topo_count == n && "topo sort missed a module");
  return true;
}

// phase 1: register declarations + resolve imports, in topological order.
static bool compiler_phase_register(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    tc_register_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }

    had_errors |= diag_has_errors(&c->diags);
  }

  // registration reads only a module's own AST, so it needs no ordering.
  // imports are linked in compiler_phase_resolve instead — see tc_link_imports.
  return !had_errors;
}

// phase 2: link imports and resolve all signatures, in topological order.
static bool compiler_phase_resolve(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    // every dependency precedes m in topological order, so its scopes are
    // already populated by the time we copy names out of them.
    tc_link_imports(&c->tc, m, &c->mod_reg);
    tc_resolve_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }

    had_errors |= diag_has_errors(&c->diags);
  }
  return !had_errors;
}

// phase 3: type-check all bodies, in topological order.
static bool compiler_phase_check(Compiler *c) {
  bool had_errors = false;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *m = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    tc_check_module(&c->tc, m);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }
    had_errors |= diag_has_errors(&c->diags);
  }

  return !had_errors;
}

// run the full compilation pipeline
bool compiler_run(Compiler *c, const char *path) {
  if (!compiler_phase_discover(c, path)) {
    fprintf(stderr, "compilation failed during discovery.\n");
    return false;
  }

  if (!compiler_phase_dep_graph(c)) {
    fprintf(stderr,
            "compilation failed during dependency graph construction.\n");
    return false;
  }

  if (!compiler_phase_register(c)) {
    fprintf(stderr, "compilation failed during registration.\n");
    return false;
  }

  if (!compiler_phase_resolve(c)) {
    fprintf(stderr, "compilation failed during resolution.\n");
    return false;
  }

  if (!compiler_phase_check(c)) {
    fprintf(stderr, "compilation failed during type checking.\n");
    return false;
  }

  return true;
}

// link and compile every checked module to bytecode, reporting the root
// module's `main`. on success the caller owns `heap` and must destroy it.
static bool compiler_codegen(Compiler *c, Executable *exe, Heap *heap,
                             bool gc_stress, FunDef **main_out) {
  // linking precedes codegen, not the other way round: a chunk can name a
  // definition from any module, so every slot must exist before the first
  // instruction is emitted — and the heap roots off the linked tables, since
  // codegen interns strings as it goes and may collect mid-compile.
  if (!exe_link(exe, &c->mod_reg, &c->al)) {
    fprintf(stderr, "compilation failed during linking.\n");
    return false;
  }

  heap_init(heap, exe, gc_stress);

  Mono mono;
  mono_init(&mono, exe, heap, &c->tc.impl_index, &c->al);

  bool ok = true;
  for (int i = 0; i < c->mod_reg.module_count; i++) {
    Module *mod = modreg_topo(&c->mod_reg, i);
    diag_clear(&c->diags);

    ok &= codegen_module(mod, &mono, &c->diags);
    // per module, so a diagnostic is reported against its own source.
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, mod->file_path.chars, mod->source.chars, stderr);
    }
  }

  // then the monomorphised copies the walk above discovered. Compiling one can
  // discover more (a generic body calling another generic), so this drains
  // rather than iterating a fixed list. Each instance is reported against the
  // module its body was written in, not the one that instantiated it.
  for (Module *mod; (mod = mono_pending_module(&mono)) != NULL;) {
    diag_clear(&c->diags);
    ok &= mono_compile_next(&mono, &c->diags);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, mod->file_path.chars, mod->source.chars, stderr);
    }
  }

  if (!ok) {
    fprintf(stderr, "compilation failed during code generation.\n");
    heap_destroy(heap);
    return false;
  }

  Module *m = c->root_module;
  FunDef *main_fn = NULL;
  for (int i = 0; i < m->fun_count; i++) {
    if (sv_equal_cstr(m->funs[i]->name, "main")) {
      main_fn = m->funs[i];
      break;
    }
  }
  if (main_fn == NULL) {
    fprintf(stderr, "error: no 'main' function to run\n");
    heap_destroy(heap);
    return false;
  }
  if (main_fn->slot == FUN_SLOT_NONE) {
    // a generic definition has no compiled body of its own, only the copies
    // its call sites ask for — and nothing calls the entry point.
    fprintf(stderr, "error: 'main' must not be generic\n");
    heap_destroy(heap);
    return false;
  }

  *main_out = main_fn;
  return true;
}

// compile every checked module to bytecode and run the root module's `main`.
// requires a successful compiler_run first.
bool compiler_execute(Compiler *c, bool gc_stress) {
  Executable exe = {0};
  Heap heap;
  FunDef *main_fn;
  if (!compiler_codegen(c, &exe, &heap, gc_stress, &main_fn)) {
    return false;
  }

  bool ok = vm_run(&exe, &heap, main_fn);
  heap_destroy(&heap);
  return ok;
}

// compile every checked module and write the linked program to `out_path` as
// a bytecode image instead of running it.
bool compiler_emit(Compiler *c, const char *out_path) {
  Executable exe = {0};
  Heap heap;
  FunDef *main_fn;
  // the heap is only here because codegen interns string literals into it;
  // bc_write copies their bytes out, so nothing survives the destroy below.
  if (!compiler_codegen(c, &exe, &heap, false, &main_fn)) {
    return false;
  }

  bool ok = bc_write(&exe, main_fn, out_path, &c->al);
  heap_destroy(&heap);
  return ok;
}

// load a bytecode image and run its recorded entry point. no compiler state is
// involved: an image is a complete program.
bool bytecode_execute(const char *path, bool gc_stress, Allocator *al) {
  Executable exe = {0};
  Heap heap;
  FunDef *entry;
  if (!bc_load(path, al, &exe, &heap, gc_stress, &entry)) {
    return false;
  }

  bool ok = vm_run(&exe, &heap, entry);
  heap_destroy(&heap);
  return ok;
}
