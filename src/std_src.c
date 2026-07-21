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
  static char buf[256];
  static bool built = false;
  if (built) {
    return buf;
  }

  size_t n = 0;
  for (int i = 0; i < STD_MODULE_COUNT; i++) {
    int written = snprintf(buf + n, sizeof(buf) - n, "%s%s", i > 0 ? ", " : "",
                           STD_MODULES[i].name);
    if (written < 0 || (size_t)written >= sizeof(buf) - n) {
      break; // truncate rather than overflow; it is only a hint
    }
    n += (size_t)written;
  }
  built = true;
  return buf;
}
