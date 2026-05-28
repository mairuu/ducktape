#pragma once

#include "allocator.h"
#include <string.h>

// ============================================================================

#define SV_FMT "%.*s"

#define SV_ARG(sv) (int)(sv).len, (sv).chars

#define STR_ARG(s) (int)(s).len, (s).chars

// ============================================================================

typedef struct {
  char *chars;
  int len;
  int cap;
} String;

typedef struct {
  const char *chars;
  int len;
} StringView;

// ============================================================================

static inline String str_null() {
  return (String){.chars = NULL, .len = 0, .cap = 0};
}

// takes ownership of chars
static inline String str_create(char *chars, int len, int cap) {
  return (String){.chars = chars, .len = len, .cap = cap};
}

void str_destroy(String *str, Allocator *al);

// ============================================================================

static inline StringView sv_from_cstr(const char *cstr) {
  return (StringView){.chars = cstr, .len = (int)strlen(cstr)};
}

bool sv_equal(StringView a, StringView b);

bool sv_equal_cstr(StringView sv, const char *cstr);