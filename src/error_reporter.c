#include "error_reporter.h"

#include <stdarg.h>
#include <stdio.h>

void reporter_init(ErrorReporter *r, const char *file, const char *source) {
  r->count = 0;
  r->had_error = false;
  r->current_file = file;
  r->source = source;
}

void reporter_error(ErrorReporter *r, int line, int line_end, int col,
                    int col_end, const char *msg, const char *fmt, ...) {
  r->had_error = true;
  if (r->count >= MAX_ERRORS)
    return; // silent once cap is hit

  CompileError *e = &r->errors[r->count++];
  e->line = line;
  e->line_end = line_end;
  e->col = col;
  e->col_end = col_end;
  e->file = r->current_file;

  snprintf(e->msg, sizeof(e->msg), "%s", msg);

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e->body, sizeof(e->body), fmt, ap);
  va_end(ap);
}

// error: {msg}
//   --> {current_file}:{line}:{col}
//    |
//  {line} | {line of source code}
//    | ^ {body}
void reporter_print_all(const ErrorReporter *r, FILE *out) {
  for (int i = 0; i < r->count; i++) {
    const CompileError *e = &r->errors[i];

    int indent = 0;
    int x = e->line_end;
    while (x) {
      indent++;
      x /= 10;
    }
    const char *indent_str = "       ";

    fprintf(out, "error: %s\n", e->msg);
    fprintf(out, "%*.s--> %s:%d:%d\n", indent, indent_str, e->file, e->line, e->col);

    fprintf(out, " %*.s |\n", indent, indent_str);
    for (int line = e->line; line <= e->line_end; line++) {
      const char *line_start = r->source;
      for (int j = 1; j < line; j++) {
        while (*line_start && *line_start != '\n')
          line_start++;
        if (*line_start)
          line_start++;
      }
      const char *line_end = line_start;
      while (*line_end && *line_end != '\n')
        line_end++;

      fprintf(out, " %*.d | %.*s\n", indent, line, (int)(line_end - line_start),
              line_start);

      int col_start = (line == e->line) ? e->col : 1;
      int col_end =
          (line == e->line_end) ? e->col_end : (int)(line_end - line_start + 1);
      fprintf(out, " %*.s | %*s", indent, indent_str, col_start - 1, "");
      for (int j = col_start; j < col_end; j++)
        fputc('^', out);
      if (line == e->line && e->body[0]) {
        fprintf(out, " %s", e->body);
      }
      fprintf(out, "\n");
    }
  }
}