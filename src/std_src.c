// ═══════════════════════════════════════════════════════════════════════════════
// std_src.c — the embedded standard library
// ═══════════════════════════════════════════════════════════════════════════════
#include "std_src.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  const char *source;
} StdModule;

// std_data.h is generated into the build directory from std/*.dt; it is a bare
// list of initialisers, so the table shape lives here rather than in the
// generator.
static const StdModule STD_MODULES[] = {
#include "std_data.h"
};

static const int STD_MODULE_COUNT =
    (int)(sizeof(STD_MODULES) / sizeof(STD_MODULES[0]));

const char *std_module_source(StringView name) {
  for (int i = 0; i < STD_MODULE_COUNT; i++) {
    if (sv_equal_cstr(name, STD_MODULES[i].name)) {
      return STD_MODULES[i].source;
    }
  }
  return NULL;
}

const char *std_module_names(void) {
  // built once into a static buffer: the only caller is a diagnostic, and the
  // list is fixed at compile time.
  static char buf[512];
  static bool built = false;
  if (built) {
    return buf;
  }

  // A nested module is *keyed* by path and *spelled* `::`, and this list is
  // read by someone about to type one — so the separator is translated here,
  // one byte to two, rather than the table carrying the user-facing form.
  size_t n = 0;
  for (int i = 0; i < STD_MODULE_COUNT && n + 1 < sizeof(buf); i++) {
    if (i > 0) {
      buf[n++] = ',';
      buf[n++] = ' ';
    }
    for (const char *c = STD_MODULES[i].name; *c != '\0'; c++) {
      if (n + 3 >= sizeof(buf)) {
        goto done; // truncate rather than overflow; it is only a hint
      }
      if (*c == '/') {
        buf[n++] = ':';
        buf[n++] = ':';
      } else {
        buf[n++] = *c;
      }
    }
  }
done:
  buf[n] = '\0';
  built = true;
  return buf;
}
