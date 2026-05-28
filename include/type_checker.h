#pragma once

#include "allocator.h"
#include "ast.h"
#include "error_reporter.h"

typedef struct {
  ErrorReporter *reporter;
  Allocator *al;

  void *internal;
} TypeChecker;

void tc_init(TypeChecker *tc, ErrorReporter *reporter, Allocator *al);
void tc_destroy(TypeChecker *tc);

void tc_check_program(TypeChecker *tc, Program *program);