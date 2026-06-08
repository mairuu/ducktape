#include "diag.h"
#include "allocator.h"
#include "string_utils.h"

#include <stdarg.h>

void diag_init(DiagBag *db, Allocator *al) {
  db->diags = NULL;
  db->count = 0;
  db->cap = 0;
  db->error_count = 0;
  db->al = al;
}

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
}

static void diag_add(DiagBag *db, DiagLevel level, Span span, const char *fmt,
                     va_list ap) {
  if (db->count >= db->cap) {
    int new_cap = db->cap == 0 ? 8 : db->cap * 2;
    db->diags = al_realloc(db->al, db->diags, sizeof(Diag) * db->cap,
                           sizeof(Diag) * new_cap);
    db->cap = new_cap;
  }

  Diag *d = &db->diags[db->count++];
  d->level = level;
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
  diag_add(db, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
}

void diag_warning(DiagBag *db, Span span, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_WARNING, span, fmt, ap);
  va_end(ap);
}

void diag_note(DiagBag *db, Span span, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_add(db, DIAG_NOTE, span, fmt, ap);
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

    fprintf(sink, "%s: %s\n",
            d->level == DIAG_ERROR
                ? "error"
                : (d->level == DIAG_WARNING ? "warning" : ""),
            d->message.chars);
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