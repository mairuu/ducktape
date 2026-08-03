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
  // every one of these owns memory *from* the arena, so the arena has to
  // outlive them: `diag_destroy` walks the diag array to free each message,
  // and both live in `c->al`. Tearing the arena down first is a use-after-free
  // — one that only shows when the last phase leaves diagnostics behind, since
  // each phase clears the bag at the top of its loop rather than the bottom.
  diag_destroy(&c->diags);
  modreg_destroy(&c->mod_reg);
  tc_destroy(&c->tc);
  // the interning table is a process global whose entries — and, now that it
  // can grow, whose array — are arena memory. Dropping it here is what keeps
  // "one compile per process" an incidental fact rather than a requirement.
  type_intern_reset();

  arena_destroy(&c->arena);
}

// does this module's advice have anyone to act on it? An error is a fact about
// the program and is always reported; a warning is addressed, and the embedded
// std compiled into every program has no reader — *as a dependency*. Naming a
// std file on the command line makes you exactly that reader, so the root
// always warns whoever it belongs to.
static bool module_has_audience(const Compiler *c, const Module *m) {
  return !mod_is_std_module(m) || m == c->root_module;
}

// start a phase's work on one module: drop what the previous module left and
// decide whether this one's warnings have an audience. Every phase loops over
// modules this way, so it is the one place that answers "whose code is this?".
static void compiler_begin_module(Compiler *c, Module *m) {
  diag_clear(&c->diags);
  diag_set_warnings(&c->diags, module_has_audience(c, m));
}

// parse one module and build the children it declares. Its own bag cycle, so a
// module parsed on another's behalf cannot lose that other's diagnostics.
static bool compiler_load_module(Compiler *c, Module *m) {
  // set before the parse rather than after: mod_parse clears the bag, and the
  // audience survives a clear precisely so this ordering is free.
  diag_set_warnings(&c->diags, module_has_audience(c, m));

  bool ok = mod_parse(m, &c->diags, &c->al);
  if (ok) {
    ok = mod_declare_children(m, &c->mod_reg, &c->diags, &c->al);
  } else {
    m->load_failed = true;
  }
  if (diag_has_diags(&c->diags)) {
    diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
  }
  return ok;
}

// the `--std-module std::a::b` lint target, made the root of the compile.
//
// The descent has to load as it goes: `b` is a name only `a` knows, and the
// lint names its target before anything has asked for it, so nothing else will
// have walked the way there.
#define MOD_PATH_MAX 8
static bool compiler_open_std_module(Compiler *c, const char *path) {
  StringView segs[MOD_PATH_MAX];
  int count = mod_split_path(path, segs, MOD_PATH_MAX);

  Module *cur = c->mod_reg.std_root;
  for (int i = 1; i < count; i++) {
    ModChild *ch = cur->ast != NULL ? mod_find_child(cur, segs[i]) : NULL;
    if (ch == NULL) {
      fprintf(stderr,
              "error: there is no standard library module '%s' ('" SV_FMT
              "' names nothing)\n",
              path, SV_ARG(segs[i]));
      return false;
    }
    cur = ch->mod;
    if (cur->ast == NULL && i + 1 < count) {
      modreg_add(&c->mod_reg, cur);
      if (!compiler_load_module(c, cur)) {
        return false;
      }
    }
  }
  if (!sv_equal_cstr(segs[0], "std")) {
    fprintf(stderr, "error: a module path must begin with 'std::'\n");
    return false;
  }

  // read the target from disk, so a lint sees the edit and not whatever `make`
  // last mirrored; its dependencies stay embedded.
  mod_read_from_disk(cur, &c->al);
  c->root_module = cur;
  modreg_add(&c->mod_reg, cur);
  return true;
}

// phase 0: build the two module trees, parse every module in them, and resolve
// every `use` against the result.
//
// Discovery cannot be one pass, because both edges out of a module are things
// only its AST holds: `mod` names its children, `use` names its dependencies.
// A path two library modules deep therefore needs the first parsed before the
// second can be *named*. So this is a fixpoint — parse everything unparsed,
// let every path register what it walks through, repeat until the registry
// stops growing — and it terminates because the tree is finite and a module
// joins the registry once.
static bool compiler_phase_discover(Compiler *c, const char *root_path,
                                    const char *std_module) {
  ModuleRegistry *reg = &c->mod_reg;
  bool had_errors = false;

  // the library root first: it is the one statement of what `std::` names, and
  // every module's prelude reads its children.
  reg->std_root = mod_new_std_root(&c->al);
  if (std_module != NULL && strcmp(std_module, "std") == 0) {
    mod_read_from_disk(reg->std_root, &c->al);
    c->root_module = reg->std_root;
  }
  modreg_add(reg, reg->std_root);
  if (!compiler_load_module(c, reg->std_root)) {
    return false;
  }

  if (std_module != NULL) {
    if (c->root_module == NULL && !compiler_open_std_module(c, std_module)) {
      return false;
    }
  } else {
    StringView root = path_normalise(sv_from_cstr(root_path), &c->al);
    reg->program_root = mod_new_program_root(root, &c->al);
    c->root_module = reg->program_root;
    modreg_add(reg, reg->program_root);
  }

  // reg->modules is realloc'd as it grows, so never cache the array; the bound
  // is re-read on every iteration of both inner loops.
  for (;;) {
    int before = reg->module_count;
    for (int i = 0; i < reg->module_count; i++) {
      Module *m = reg->modules[i];
      if (m->ast != NULL || m->load_failed) {
        continue;
      }
      had_errors |= !compiler_load_module(c, m);
    }
    for (int i = 0; i < reg->module_count; i++) {
      if (reg->modules[i]->ast != NULL) {
        mod_reach_imports(reg->modules[i], reg);
      }
    }
    if (reg->module_count == before) {
      break;
    }
  }

  // the tree is final, so a path that resolved to nothing really names nothing.
  for (int i = 0; i < reg->module_count; i++) {
    Module *m = reg->modules[i];
    if (m->ast == NULL) {
      continue;
    }
    compiler_begin_module(c, m);
    had_errors |= !mod_collect_imports(m, reg, &c->diags, &c->al);
    // every non-std module implicitly depends on the prelude, appended after
    // its own imports so those link first and win a name clash.
    mod_inject_prelude(m, reg, &c->al);
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
              SV_ARG(mod_label(from)), SV_ARG(mod_label(to)));
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
    if (imp->module_index < 0) {
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
    compiler_begin_module(c, m);

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
    compiler_begin_module(c, m);

    // every dependency precedes m in topological order, so its scopes and its
    // visible impl set are already populated by the time we copy out of them.
    tc_link_imports(&c->tc, m, &c->mod_reg);
    tc_import_impls(&c->tc, m, &c->mod_reg);
    tc_resolve_module(&c->tc, m);
    // after resolve: a variant import qualified by a *name* resolves that name
    // against m's own type scope, which is complete only now.
    tc_link_scope_imports(&c->tc, m, &c->mod_reg);
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
    compiler_begin_module(c, m);

    tc_check_module(&c->tc, m);
    tc_report_unused_imports(&c->tc, m, &c->mod_reg);
    if (diag_has_diags(&c->diags)) {
      diag_report(&c->diags, m->file_path.chars, m->source.chars, stderr);
    }
    had_errors |= diag_has_errors(&c->diags);
  }

  return !had_errors;
}

// run the full compilation pipeline
bool compiler_run(Compiler *c, const char *path, const char *std_module) {
  if (!compiler_phase_discover(c, path, std_module)) {
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

// the root module's `main`, checked for the two shapes that cannot be an entry
// point. Found before codegen rather than after, because it *is* codegen's
// starting point: nothing else in the program is compiled except through what
// it reaches.
static FunDef *compiler_find_main(Compiler *c) {
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
    return NULL;
  }
  if (fun_is_native(main_fn)) {
    // the VM enters a frame over `main`'s chunk, and a native has none.
    fprintf(stderr, "error: 'main' must not be native\n");
    return NULL;
  }
  if (fun_is_generic(main_fn)) {
    // a generic definition has no body of its own, only the copies its call
    // sites ask for — and nothing calls the entry point.
    fprintf(stderr, "error: 'main' must not be generic\n");
    return NULL;
  }
  return main_fn;
}

// link and compile the reachable part of the program to bytecode, reporting
// the root module's `main`. on success the caller owns `heap` and must destroy
// it.
static bool compiler_codegen(Compiler *c, Executable *exe, Heap *heap,
                             bool gc_stress, FunDef **main_out) {
  FunDef *main_fn = compiler_find_main(c);
  if (main_fn == NULL) {
    return false;
  }

  // linking precedes codegen, not the other way round: the heap roots off
  // these tables, and codegen interns strings as it goes so it may collect
  // mid-compile. They start empty — codegen fills them as it discovers what
  // the program actually names.
  exe_assign_tags(&c->mod_reg);
  heap_init(heap, exe, gc_stress);

  Mono mono;
  mono_init(&mono, exe, heap, &c->al);

  // Seed the walk with `main` and drain. Compiling one body discovers more —
  // its calls, the vtables its coercions build, the generics it instantiates —
  // so this is a worklist rather than a pass over the registry, and a module
  // no reachable body names is never visited at all. Each body is reported
  // against the module it was *written* in, not the one that reached it.
  bool ok = mono_seed(&mono, main_fn);
  for (Module *mod; (mod = mono_pending_module(&mono)) != NULL;) {
    compiler_begin_module(c, mod);
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
