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

// Every warning has a name, because suppressing one means *asking about* it and
// a format string cannot be asked about. The name is what `@allow("…")` matches
// and what `warning[…]:` prints, so the spelling a reader sees is the spelling
// they write — one table, no second list to drift.
typedef enum {
  LINT_UNUSED_VARIABLE,
  LINT_UNUSED_IMPORT,
  LINT_UNREACHABLE_CODE,
  LINT_IRREFUTABLE_PATTERN,
  LINT_COUNT,
} DiagLint;

#define LINT_BIT(lint) (1u << (unsigned)(lint))

typedef struct {
  DiagLevel level;
  DiagLint lint; // which one, for `warning[…]:`; LINT_COUNT on anything but a
                 // DIAG_WARNING, since only a warning can be asked about
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
  // the lints `@allow` has turned off for the declaration being walked right
  // now. The second policy through the same door as the first: an audience asks
  // whether anyone can act on the advice, an allow asks whether they want it.
  unsigned allowed;
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

// -1 if no lint is called that. `diag_lint_names` is the "available: …" line.
int diag_lint_from_name(StringView name);
const char *diag_lint_name(DiagLint lint);
const char *diag_lint_names(void);

// entering a declaration adds its allows to the ones already in force and
// returns what to restore on the way out — an `@allow` on an `impl` covers the
// methods inside it, so the sets nest by union rather than replacing.
unsigned diag_push_allowed(DiagBag *db, unsigned add);
void diag_pop_allowed(DiagBag *db, unsigned saved);

void diag_error(DiagBag *db, Span span, const char *fmt, ...);
void diag_warning(DiagBag *db, DiagLint lint, Span span, const char *fmt, ...);
void diag_note(DiagBag *db, Span span, const char *fmt, ...);
bool diag_has_errors(const DiagBag *db);
bool diag_has_diags(const DiagBag *db);

void diag_report(const DiagBag *db, const char *source_name, const char *source,
                 FILE *sink);