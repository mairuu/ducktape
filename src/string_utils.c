#include "string_utils.h"

void str_destroy(String *str, Allocator *al) {
  if (str->chars == NULL) {
    return;
  }
  al_free(al, str->chars, sizeof(char) * str->cap);
  str->chars = NULL;
  str->len = 0;
  str->cap = 0;
}

bool sv_equal(StringView a, StringView b) {
  // The length test has to come first, and not only as a shortcut: an *empty*
  // view may legitimately point nowhere — `(StringView){0}` is how an absent
  // name is spelled (a closure's, a fresh unknown's) — and `memcmp` is not
  // defined on a NULL pointer even for zero bytes. Unlike an allocation, a
  // view has no address of its own to fall back on: it is a pointer *into*
  // something, and when there is nothing, there is no pointer.
  return a.len == b.len && (a.len == 0 || memcmp(a.chars, b.chars, a.len) == 0);
}

bool sv_equal_cstr(StringView sv, const char *cstr) {
  for (int i = 0; i < sv.len; i++) {
    if (cstr[i] == '\0' || cstr[i] != sv.chars[i]) {
      return false;
    }
  }
  return cstr[sv.len] == '\0';
}