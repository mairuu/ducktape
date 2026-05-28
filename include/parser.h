#pragma once

#include "allocator.h"
#include "ast.h"
#include "error_reporter.h"
#include "scanner.h"

typedef struct {
  Token *tokens;
  int count;
  int current;

  Allocator *al;
  ErrorReporter *reporter;

  bool allow_struct_init;
  bool panic_mode;
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Allocator *al,
                 ErrorReporter *reporter);

Program *parser_parse(Parser *p);
