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
  return a.len == b.len && memcmp(a.chars, b.chars, a.len) == 0;
}

bool sv_equal_cstr(StringView sv, const char *cstr) {
  for (int i = 0; i < sv.len; i++) {
    if (cstr[i] == '\0' || cstr[i] != sv.chars[i]) {
      return false;
    }
  }
  return cstr[sv.len] == '\0';
}