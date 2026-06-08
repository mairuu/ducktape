#pragma once

#include "ast.h"
#include "sema.h"

typedef struct Module Module;

struct Module {
  StringView file_path; // absolute filesystem path, null-terminated
  String source;
  Program *ast;

  FunDef **funs;
  int fun_count, fun_cap;

  ValueScope *vscope; // module-level values: functions, global vars
};

bool mod_parse(Module *m, DiagBag *diags, Allocator *al);

// todo: module registry