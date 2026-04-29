#include "string_utils.h"

#include <string.h>

#include "allocator.h"

String str_from_cstr(const char *cstr, Allocator *al) {
  int length = strlen(cstr);
  char *chars = al_alloc(al, length + 1);
  memcpy(chars, cstr, length + 1);
  return (String){.chars = chars, .length = length, .capacity = length + 1};
}

String str_from_sv(StringView sv, Allocator *al) {
  char *chars = al_alloc(al, sv.length + 1);
  memcpy(chars, sv.chars, sv.length);
  chars[sv.length] = '\0';
  return (String){.chars = chars, .length = sv.length, .capacity = sv.length + 1};
}

String str_copy(const String *str, Allocator *al) {
  char *chars = al_alloc(al, str->capacity);
  memcpy(chars, str->chars, str->capacity);
  return (String){.chars = chars, .length = str->length, .capacity = str->capacity};
}

String str_copy_sv(StringView sv, Allocator *al) {
  char *chars = al_alloc(al, sv.length + 1);
  memcpy(chars, sv.chars, sv.length);
  chars[sv.length] = '\0';
  return (String){.chars = chars, .length = sv.length, .capacity = sv.length + 1};
}

String str_concat(const String *a, const String *b, Allocator *al) {
  int length = a->length + b->length;
  char *chars = al_alloc(al, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';
  return (String){.chars = chars, .length = length, .capacity = length + 1};
}

void str_destroy(String *str, Allocator *al) {
  al_free(al, str->chars, str->capacity);
  str->chars = NULL;
  str->length = 0;
  str->capacity = 0;
}

// -----------------------------------------------------------------------------

StringView sv_from_cstr(const char *cstr) {
  int length = strlen(cstr);
  return (StringView){.chars = cstr, .length = length};
}

StringView sv_from_str(String str) {
  return (StringView){.chars = str.chars, .length = str.length};
}

bool sv_equal(StringView a, StringView b) {
  return a.length == b.length && memcmp(a.chars, b.chars, a.length) == 0;
}

uint32_t sv_hash(StringView sv) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < sv.length; i++) {
    hash ^= (uint8_t)sv.chars[i];
    hash *= 16777619u;
  }
  return hash;
}