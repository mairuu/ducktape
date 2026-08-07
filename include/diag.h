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
  LINT_UNUSED_LABEL,
  LINT_UNREACHABLE_CODE,
  LINT_IRREFUTABLE_PATTERN,
  LINT_ORPHAN_MODULE,
  LINT_COUNT,
} DiagLint;

#define LINT_BIT(lint) (1u << (unsigned)(lint))

// What a lint is worth, from the outside. `@allow` is a decision written beside
// the code; this is the build's, and the two meet at the emission door.
//
// One value rather than an enabled flag plus a fatal flag, which is what makes
// the flag set small: "stop being fatal" and "be a warning" are the same
// request, so `-W<lint>` spells both and there is no `-Wno-error=` to write.
typedef enum {
  LINT_LEVEL_ALLOW,
  LINT_LEVEL_WARN,
  LINT_LEVEL_ERROR,
} LintLevel;

typedef struct {
  unsigned char level[LINT_COUNT];
} LintLevels;

void lint_levels_init(LintLevels *ls); // every lint at LINT_LEVEL_WARN

// apply one `-W…` argument, last one winning. The four are `-Werror` (all of
// them), `-Werror=<lint>`, `-Wno-<lint>` and `-W<lint>`. False if it is not a
// `-W` flag or names no lint; the caller owns the message, since a command
// line is not source and gets no span.
bool lint_levels_flag(LintLevels *ls, const char *arg);

typedef struct {
  DiagLevel level;
  DiagLint lint; // which one, for the `warning[…]:`/`error[…]:` header. Not
                 // implied by the level: `-Werror` gives a lint DIAG_ERROR.
                 // LINT_COUNT where nothing can be asked about.
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
  // what the command line asked for, per lint. Static where `allowed` is
  // dynamically scoped, and consulted *after* it: a flag is a blanket, and the
  // author of one line knew something a blanket cannot.
  LintLevels lints;
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
void diag_set_lints(DiagBag *db, const LintLevels *ls);

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
