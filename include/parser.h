#pragma once

#include "allocator.h"
#include "ast.h"
#include "diagbag.h"
#include "scanner.h"

typedef struct {
  Token *tokens;
  int count;
  int current;

  Allocator *al;
  DiagBag *diags;

  bool allow_struct_init;
  bool panic_mode;
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Allocator *al,
                 DiagBag *diags);

Program *parser_parse(Parser *p);
