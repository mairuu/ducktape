#pragma once

#include "string_utils.h"

#include <stdio.h>

typedef struct {
  int line;
  int line_end;
  int col;
  int col_end;
} Span;

typedef enum {
  DIAG_ERROR,
  DIAG_WARNING,
  DIAG_NOTE, // attached to a prior error (same index in DiagBag)
} DiagLevel;

typedef struct {
  DiagLevel level;
  Span span;
  String message;
} Diag;

typedef struct {
  Diag *diags;
  int count;
  int cap;
  int error_count;
  // an error is a fact about the program; a warning is advice to whoever wrote
  // it. Code with no such author — the embedded std, compiled from source into
  // every program — has nobody to advise, so its warnings are dropped at the
  // source rather than filtered at report time, keeping `diag_has_diags`
  // honest.
  bool warnings_enabled;
  // a note attaches to the diagnostic before it by position, so dropping a
  // warning has to drop the notes it came with or they orphan onto whatever
  // was reported last.
  bool last_dropped;
  Allocator *al;
} DiagBag;

void diag_init(DiagBag *db, Allocator *al);
void diag_destroy(DiagBag *db);

void diag_clear(DiagBag *db);
void diag_set_warnings(DiagBag *db, bool enabled);

void diag_error(DiagBag *db, Span span, const char *fmt, ...);
void diag_warning(DiagBag *db, Span span, const char *fmt, ...);
void diag_note(DiagBag *db, Span span, const char *fmt, ...);
bool diag_has_errors(const DiagBag *db);
bool diag_has_diags(const DiagBag *db);

void diag_report(const DiagBag *db, const char *source_name, const char *source,
                 FILE *sink);