#include "diag.h"
#include "allocator.h"
#include "string_utils.h"

#include <stdarg.h>

void diag_init(DiagBag *db, Allocator *al) {
  db->diags = NULL;
  db->count = 0;
  db->cap = 0;
  db->error_count = 0;
  db->warnings_enabled = true;
  db->allowed = 0;
  db->last_dropped = false;
  db->al = al;
}

// the one list. Index by DiagLint; the order is the enum's.
static const char *const lint_names[LINT_COUNT] = {
    [LINT_UNUSED_VARIABLE] = "unused_variable",
    [LINT_UNUSED_IMPORT] = "unused_import",
    [LINT_UNREACHABLE_CODE] = "unreachable_code",
    [LINT_IRREFUTABLE_PATTERN] = "irrefutable_pattern",
};

const char *diag_lint_name(DiagLint lint) { return lint_names[lint]; }

int diag_lint_from_name(StringView name) {
  for (int i = 0; i < LINT_COUNT; i++) {
    if (sv_equal_cstr(name, lint_names[i])) {
      return i;
    }
  }
  return -1;
}

const char *diag_lint_names(void) {
  static char buf[256];
  size_t n = 0;
  for (int i = 0; i < LINT_COUNT && n + 1 < sizeof buf; i++) {
    int w = snprintf(buf + n, sizeof buf - n, "%s%s", i > 0 ? ", " : "",
                     lint_names[i]);
    if (w < 0 || (size_t)w >= sizeof buf - n) {
      break;
    }
    n += (size_t)w;
  }
  return buf;
}

unsigned diag_push_allowed(DiagBag *db, unsigned add) {
  unsigned saved = db->allowed;
  db->allowed |= add;
  return saved;
}

void diag_pop_allowed(DiagBag *db, unsigned saved) { db->allowed = saved; }

void diag_destroy(DiagBag *db) {
  diag_clear(db);
  al_free(db->al, db->diags, sizeof(Diag) * db->cap);
  diag_init(db, NULL);
}

void diag_clear(DiagBag *db) {
  for (int i = 0; i < db->count; i++) {
    str_destroy(&db->diags[i].message, db->al);
  }
  db->count = 0;
  db->error_count = 0;
  // an allow's scope is one declaration, which never spans a clear: every
  // caller of this is at a module boundary, where nothing is being walked.
  db->allowed = 0;
  db->last_dropped = false;
}

static void diag_add(DiagBag *db, DiagLevel level, DiagLint lint, Span span,
                     const char *fmt, va_list ap) {
  if (db->count >= db->cap) {
    int new_cap = db->cap == 0 ? 8 : db->cap * 2;
    db->diags = al_realloc(db->al, db->diags, sizeof(Diag) * db->cap,
                           sizeof(Diag) * new_cap);
    db->cap = new_cap;
  }

  db->last_dropped = false;
  Diag *d = &db->diags[db->count++];
  d->level = level;
  d->lint = lint;
  d->span = span;

  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  d->message = (String){0};
  d->message.len = strlen(buf);
  d->message.cap = d->message.len + 1;
  d->message.chars = al_alloc(db->al, d->message.cap);
  memcpy(d->message.chars, buf, d->message.cap);

  if (level == DIAG_ERROR)
    db->error_count++;
}

void diag_error(DiagBag *db, Span span, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_ERROR, LINT_COUNT, span, fmt, ap);
  va_end(ap);
}

// survives diag_clear: the audience is a property of the module being compiled,
// which outlives any one phase's bag.
void diag_set_warnings(DiagBag *db, bool enabled) {
  db->warnings_enabled = enabled;
}

// two policies, one door: no audience to advise, or an audience that has said
// it does not want this one. Either way the drop happens here rather than at
// report time, so `diag_has_diags` stays honest and the notes below it go with
// it.
void diag_warning(DiagBag *db, DiagLint lint, Span span, const char *fmt, ...) {
  if (!db->warnings_enabled || (db->allowed & LINT_BIT(lint)) != 0) {
    db->last_dropped = true;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_WARNING, lint, span, fmt, ap);
  va_end(ap);
}

void diag_note(DiagBag *db, Span span, const char *fmt, ...) {
  if (db->last_dropped) {
    return; // its subject was never reported
  }
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_NOTE, LINT_COUNT, span, fmt, ap);
  va_end(ap);
}

bool diag_has_errors(const DiagBag *db) { return db->error_count > 0; }

bool diag_has_diags(const DiagBag *db) { return db->count > 0; }

void diag_report(const DiagBag *db, const char *source_name, const char *source,
                 FILE *sink) {
  const char *indent_str = "       ";
  for (int i = 0; i < db->count; i++) {
    const Diag *d = &db->diags[i];

    if (d->level == DIAG_NOTE) {
      fprintf(sink, "> note: %s\n", d->message.chars);
      continue;
    }

    int indent = 0;
    int x = d->span.line_end;
    while (x) {
      indent++;
      x /= 10;
    }

    // a warning carries its lint name into the output: it is the name
    // `@allow("…")` takes, so reading one is how you learn to silence it.
    if (d->level == DIAG_WARNING) {
      fprintf(sink, "warning[%s]: %s\n", diag_lint_name(d->lint),
              d->message.chars);
    } else {
      fprintf(sink, "error: %s\n", d->message.chars);
    }
    fprintf(sink, "%*.s--> %s:%d:%d\n", indent, indent_str, source_name,
            d->span.line, d->span.col);

    fprintf(sink, " %*.s |\n", indent, indent_str);
    for (int line = d->span.line; line <= d->span.line_end; line++) {
      const char *line_start = source;
      for (int j = 1; j < line; j++) {
        while (*line_start && *line_start != '\n')
          line_start++;
        if (*line_start)
          line_start++;
      }
      const char *line_end = line_start;
      while (*line_end && *line_end != '\n')
        line_end++;

      fprintf(sink, " %*.d | %.*s\n", indent, line,
              (int)(line_end - line_start), line_start);

      int col_start = (line == d->span.line) ? d->span.col : 1;
      int col_end = (line == d->span.line_end)
                        ? d->span.col_end
                        : (int)(line_end - line_start + 1);
      fprintf(sink, " %*.s | %*s", indent, indent_str, col_start - 1, "");
      for (int j = col_start; j < col_end; j++)
        fputc('^', sink);
      fprintf(sink, "\n");
    }
  }
}