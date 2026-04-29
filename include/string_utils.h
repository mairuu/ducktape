#pragma once

#include <stdint.h>

#include "allocator.h"

typedef struct {
  char *chars;
  int length;
  int capacity;
} String;

typedef struct {
  const char *chars;
  int length;
} StringView;

String str_from_cstr(const char *cstr, Allocator *al);

String str_from_sv(StringView sv, Allocator *al);

String str_copy(const String *str, Allocator *al);

String str_copy_sv(StringView sv, Allocator *al);

String str_concat(const String *a, const String *b, Allocator *al);

void str_destroy(String *str, Allocator *al);

// -----------------------------------------------------------------------------

StringView sv_from_cstr(const char *cstr);

StringView sv_from_str(String str);

bool sv_equal(StringView a, StringView b);

uint32_t sv_hash(StringView sv);