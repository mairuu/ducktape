#include "repl.h"
#include "codegen.h"
#include "compiler.h"
#include "module.h"
#include "sema.h"
#include "string_utils.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════════════════════
// REPL
// ═══════════════════════════════════════════════════════════════════════════════
//
// A session is *one* module that grows a line at a time. Everything already
// entered stays registered, resolved and compiled; `Module.decl_base` is what
// makes each phase run over the new declarations only, so a function defined
// on line 2 is an ordinary module-level function to line 7 — no re-checking,
// no re-running, and no second name-resolution path.
//
// The heap and the `Executable` outlive every line: `exe_link` is re-run each
// time (definitions are only appended, so previously assigned slots come back
// identical), and the chunks compiled for earlier lines stay valid and stay
// rooted.
//
// A line that is not a declaration is wrapped into a synthetic function and
// executed. That is the whole trick: it means the REPL evaluates statements
// with the *function* machinery — locals, closures, control flow all work —
// instead of needing a top-level execution mode the compiler doesn't have.

typedef struct {
  Compiler *c;
  Module *m;
  Executable exe;
  Heap heap;
  int line_no;
  bool warned_var;
} Repl;

// ── session rollback ─────────────────────────────────────────────────────────

// A rejected line must leave no trace: the phases append to the module's decl
// list, its definition tables and its scopes as they go, and a failure can
// happen at any point in the middle. Every one of those is an array with a
// count, so a mark/restore of the counts is a complete undo — the arena memory
// beneath them is simply left behind (a session is short, and an arena has no
// free anyway).
typedef struct {
  int decls;
  int funs, fun_cap, structs, struct_cap, enums, enum_cap, impls, impl_cap;
  int vscope, tscope;
  int tc_funs, impl_index;
} ReplMark;

static ReplMark repl_mark(Repl *r) {
  Module *m = r->m;
  return (ReplMark){
      .decls = m->ast == NULL ? 0 : m->ast->decl_count,
      .funs = m->fun_count,
      .fun_cap = m->fun_cap,
      .structs = m->struct_count,
      .struct_cap = m->struct_cap,
      .enums = m->enum_count,
      .enum_cap = m->enum_cap,
      .impls = m->impl_count,
      .impl_cap = m->impl_cap,
      .vscope = m->vscope.count,
      .tscope = m->tscope.count,
      .tc_funs = r->c->tc.fun_count,
      .impl_index = r->c->tc.impl_index.count,
  };
}

static void repl_restore(Repl *r, const ReplMark *mark) {
  Module *m = r->m;
  if (m->ast != NULL) {
    m->ast->decl_count = mark->decls;
  }
  m->fun_count = mark->funs;
  m->fun_cap = mark->fun_cap;
  m->struct_count = mark->structs;
  m->struct_cap = mark->struct_cap;
  m->enum_count = mark->enums;
  m->enum_cap = mark->enum_cap;
  m->impl_count = mark->impls;
  m->impl_cap = mark->impl_cap;
  m->vscope.count = mark->vscope;
  m->tscope.count = mark->tscope;
  r->c->tc.fun_count = mark->tc_funs;
  r->c->tc.impl_index.count = mark->impl_index;

  // the linked tables still point at the definitions just dropped; re-link so
  // the heap roots only what the session actually kept.
  exe_link(&r->exe, &r->c->mod_reg, &r->c->al);
}

// ── input ────────────────────────────────────────────────────────────────────

typedef struct {
  char *chars;
  int len, cap;
  Allocator *al;
} Buf;

static void buf_append(Buf *b, const char *src, int len) {
  if (b->len + len + 1 > b->cap) {
    int new_cap = b->cap == 0 ? 128 : b->cap;
    while (b->len + len + 1 > new_cap) {
      new_cap *= 2;
    }
    b->chars = al_realloc(b->al, b->chars, (size_t)b->cap, (size_t)new_cap);
    b->cap = new_cap;
  }
  memcpy(b->chars + b->len, src, (size_t)len);
  b->len += len;
  b->chars[b->len] = '\0';
}

// how far the text is inside brackets, so an unfinished `fun f() {` keeps
// reading. Comments and string literals are skipped wholesale, which is also
// what keeps an interpolation's `{` from counting.
static int bracket_depth(const char *s, int len) {
  int depth = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] == '#') {
      while (i < len && s[i] != '\n') {
        i++;
      }
    } else if (s[i] == '"') {
      for (i++; i < len && s[i] != '"'; i++) {
        if (s[i] == '\\') {
          i++;
        }
      }
    } else if (s[i] == '{' || s[i] == '(' || s[i] == '[') {
      depth++;
    } else if (s[i] == '}' || s[i] == ')' || s[i] == ']') {
      depth--;
    }
  }
  return depth;
}

// read one submission: lines until the brackets balance, or EOF.
// returns false at EOF with nothing to evaluate.
//
// prompts go to stderr, not stdout: a session's stdout is then exactly what
// the evaluated code printed, which is what makes a piped transcript
// comparable (`tests/repl`) and a pipeline usable.
static bool read_submission(Buf *b, bool *eof) {
  char chunk[1024];
  b->len = 0;
  for (;;) {
    fputs(b->len == 0 ? "> " : "... ", stderr);

    if (fgets(chunk, sizeof chunk, stdin) == NULL) {
      *eof = true;
      fputc('\n', stderr);
      return b->len > 0;
    }
    buf_append(b, chunk, (int)strlen(chunk));

    if (bracket_depth(b->chars, b->len) <= 0) {
      return true;
    }
  }
}

// ── line shape ───────────────────────────────────────────────────────────────

static StringView sv_trim(StringView s) {
  while (s.len > 0 && (unsigned char)s.chars[0] <= ' ') {
    s.chars++;
    s.len--;
  }
  while (s.len > 0 && (unsigned char)s.chars[s.len - 1] <= ' ') {
    s.len--;
  }
  return s;
}

static bool starts_with_word(StringView s, const char *word) {
  int n = (int)strlen(word);
  if (s.len < n || memcmp(s.chars, word, (size_t)n) != 0) {
    return false;
  }
  return s.len == n ||
         !((s.chars[n] >= 'a' && s.chars[n] <= 'z') ||
           (s.chars[n] >= 'A' && s.chars[n] <= 'Z') ||
           (s.chars[n] >= '0' && s.chars[n] <= '9') || s.chars[n] == '_');
}

// nothing but comments and whitespace. Worth spotting: such a submission would
// otherwise compile to an empty function and burn one of the 256 global slots.
static bool is_all_comment(StringView s) {
  for (int i = 0; i < s.len; i++) {
    if (s.chars[i] == '#') {
      while (i < s.len && s.chars[i] != '\n') {
        i++;
      }
    } else if ((unsigned char)s.chars[i] > ' ') {
      return false;
    }
  }
  return true;
}

// a submission is either top-level declarations (kept, nothing runs) or
// statements (wrapped in a function and run). The keyword decides, exactly as
// the parser would.
static bool is_declaration(StringView s) {
  static const char *kw[] = {"fun", "struct", "enum", "trait", "impl", "pub"};
  for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++) {
    if (starts_with_word(s, kw[i])) {
      return true;
    }
  }
  return false;
}

// split a submission into leading statements and a trailing expression, at the
// last `;` that is not inside brackets or a string. `var x = 2; x * x` is then
// statements plus something worth displaying, which is the shape most lines
// typed at a prompt actually have.
static void split_tail(StringView s, StringView *head, StringView *tail) {
  int depth = 0, cut = -1;
  for (int i = 0; i < s.len; i++) {
    if (s.chars[i] == '#') {
      while (i < s.len && s.chars[i] != '\n') {
        i++;
      }
    } else if (s.chars[i] == '"') {
      for (i++; i < s.len && s.chars[i] != '"'; i++) {
        if (s.chars[i] == '\\') {
          i++;
        }
      }
    } else if (s.chars[i] == '{' || s.chars[i] == '(' || s.chars[i] == '[') {
      depth++;
    } else if (s.chars[i] == '}' || s.chars[i] == ')' || s.chars[i] == ']') {
      depth--;
    } else if (s.chars[i] == ';' && depth == 0) {
      cut = i;
    }
  }
  *head = (StringView){.chars = s.chars, .len = cut + 1};
  *tail = sv_trim(
      (StringView){.chars = s.chars + cut + 1, .len = s.len - (cut + 1)});
}

// build "fun <name>() { <body> }" in arena memory. It must be arena memory and
// must not be freed: every identifier the parser produces is a StringView into
// this buffer, so it backs the names of everything the line defines.
static String wrap_source(Repl *r, StringView head, StringView tail,
                          const char *name, bool print_tail) {
  Buf b = {.al = &r->c->al};
  buf_append(&b, "fun ", 4);
  buf_append(&b, name, (int)strlen(name));
  buf_append(&b, "() {\n", 5);
  buf_append(&b, head.chars, head.len);
  if (tail.len > 0) {
    buf_append(&b, "\n", 1);
    if (print_tail) {
      buf_append(&b, "print(", 6);
    }
    buf_append(&b, tail.chars, tail.len);
    if (print_tail) {
      buf_append(&b, ");", 2);
    }
  }
  buf_append(&b, "\n}\n", 3);
  return str_create(b.chars, b.len, b.cap);
}

static String plain_source(Repl *r, StringView body) {
  Buf b = {.al = &r->c->al};
  buf_append(&b, body.chars, body.len);
  buf_append(&b, "\n", 1);
  return str_create(b.chars, b.len, b.cap);
}

// ── evaluation ───────────────────────────────────────────────────────────────

static void report(Repl *r) {
  Module *m = r->m;
  diag_report(&r->c->diags, m->file_path.chars, m->source.chars, stderr);
}

// compile one submission's source into the session. `quiet` swallows the
// diagnostics of a speculative attempt (see repl_eval).
static bool repl_compile(Repl *r, String source, bool quiet) {
  Module *m = r->m;
  DiagBag *d = &r->c->diags;
  ReplMark mark = repl_mark(r);

  bool ok = mod_parse_append(m, source, d, &r->c->al);
  if (ok) {
    tc_register_module(&r->c->tc, m);
    ok = !diag_has_errors(d);
  }
  if (ok) {
    tc_resolve_module(&r->c->tc, m);
    ok = !diag_has_errors(d);
  }
  if (ok) {
    tc_check_module(&r->c->tc, m);
    ok = !diag_has_errors(d);
  }
  if (ok) {
    // linking before codegen, as always: the new definitions need slots before
    // any chunk names them. Re-linking is safe because slots only ever grow.
    ok = exe_link(&r->exe, &r->c->mod_reg, &r->c->al) &&
         codegen_module(m, &r->exe, &r->heap, d, &r->c->al);
  }

  if (diag_has_diags(d)) {
    if (ok || !quiet) {
      report(r);
    } else {
      diag_clear(d);
    }
  }
  if (!ok) {
    repl_restore(r, &mark);
    return false;
  }

  m->decl_base = m->ast->decl_count;
  return true;
}

// the function a wrapped submission compiled to: registration appended it, so
// it is the module's last.
static FunDef *last_fun(Repl *r) {
  return r->m->fun_count > 0 ? r->m->funs[r->m->fun_count - 1] : NULL;
}

static void repl_eval(Repl *r, StringView text) {
  if (starts_with_word(text, "use")) {
    fprintf(stderr, "error: 'use' is not supported in the REPL\n");
    return;
  }
  if (starts_with_word(text, "var") && !r->warned_var) {
    // it compiles and runs — it is just a local of this line's function, so
    // the binding is gone by the next prompt. Say so once rather than letting
    // the next line report an undefined variable.
    fprintf(stderr, "note: a 'var' binding lasts only for the line that "
                    "declares it\n");
    r->warned_var = true;
  }

  if (is_declaration(text)) {
    repl_compile(r, plain_source(r, text), false);
    return;
  }

  char name[32];
  snprintf(name, sizeof name, "__repl%d", r->line_no++);

  // a trailing expression is worth displaying, a trailing statement is not —
  // and the difference is not something the typist can be asked to annotate.
  // So the tail is *speculatively* compiled as `print(<tail>)`; if that
  // doesn't compile, the same text is compiled again untouched and those are
  // the diagnostics reported. A wrong guess costs a discarded parse, never a
  // wrong message.
  StringView head, tail;
  split_tail(sv_trim(text), &head, &tail);

  if (tail.len > 0 &&
      repl_compile(r, wrap_source(r, head, tail, name, true), true)) {
    vm_run(&r->exe, &r->heap, last_fun(r));
    return;
  }

  if (repl_compile(r, wrap_source(r, head, tail, name, false), false)) {
    vm_run(&r->exe, &r->heap, last_fun(r));
  }
}

// ── driver ───────────────────────────────────────────────────────────────────

static void print_help(void) {
  fputs("  :help          show this\n"
        "  :quit          leave (or Ctrl-D)\n"
        "declarations (fun/struct/enum/trait/impl) persist for the session;\n"
        "anything else runs, and a trailing expression prints its value.\n",
        stderr);
}

bool repl_run(Allocator *al, bool gc_stress) {
  Compiler c;
  compiler_init(&c, al);

  Repl r = {.c = &c};
  r.m = mod_new(sv_from_cstr("<repl>"), &c.al);
  modreg_add(&c.mod_reg, r.m);
  // one module, so the topological order the linker walks is trivial — but it
  // still has to exist.
  c.mod_reg.topo_order = al_alloc(&c.al, sizeof(int));
  c.mod_reg.topo_order[0] = 0;
  c.mod_reg.topo_count = 1;

  // an empty session is a valid program: register it so the builtins exist,
  // link the (empty) tables, and root the heap off them before any line can
  // intern a string constant.
  mod_parse_append(r.m, plain_source(&r, (StringView){.chars = "", .len = 0}),
                   &c.diags, &c.al);
  tc_register_module(&c.tc, r.m);
  r.m->decl_base = r.m->ast->decl_count;
  exe_link(&r.exe, &c.mod_reg, &c.al);
  heap_init(&r.heap, &r.exe, gc_stress);

  fputs("ducktape REPL — :help for help, :quit to leave\n", stderr);

  Buf input = {.al = al};
  bool eof = false;
  while (!eof) {
    if (!read_submission(&input, &eof)) {
      break;
    }
    StringView text =
        sv_trim((StringView){.chars = input.chars, .len = input.len});
    if (text.len == 0 || is_all_comment(text)) {
      continue;
    }
    if (starts_with_word(text, ":quit") || starts_with_word(text, ":q")) {
      break;
    }
    if (starts_with_word(text, ":help")) {
      print_help();
      continue;
    }
    repl_eval(&r, text);
  }

  if (input.chars != NULL) {
    al_free(al, input.chars, (size_t)input.cap);
  }
  heap_destroy(&r.heap);
  compiler_destroy(&c, al);
  return true;
}
