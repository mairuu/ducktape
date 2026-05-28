#pragma once

#include <stdio.h>

#define MAX_ERRORS 64

typedef struct {
  int line;
  int line_end;
  int col;
  int col_end;
  const char *file;
  char msg[128];
  char body[256];
} CompileError;

typedef struct {
  CompileError errors[MAX_ERRORS];
  int count;
  bool had_error;

  const char *current_file;
  const char *source;
} ErrorReporter;

void reporter_init(ErrorReporter *r, const char *file, const char *sourece);

void reporter_error(ErrorReporter *r, int line, int line_end, int col,
                    int col_end, const char *msg, const char *fmt, ...);

void reporter_print_all(const ErrorReporter *r, FILE *out);