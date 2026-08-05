#pragma once

#include "allocator.h"
#include <stdint.h>
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

// ============================================================================
// UTF-8
//
// A `String` is bytes and a `char` is a Unicode scalar value, so these two
// functions are the only bridge between the two views. Both are strict: an
// overlong encoding, a surrogate and anything past U+10FFFF are rejected
// rather than round-tripped, so a `char` that exists is always a scalar value
// and `utf8_encode` on one always succeeds.

#define UTF8_MAX_BYTES 4

static inline bool utf8_is_scalar(uint32_t cp) {
  return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

// writes 1..4 bytes into `out`; returns how many, or 0 if `cp` is not a
// scalar value.
int utf8_encode(uint32_t cp, char *out);

// reads one scalar value from `p[0..len)`; returns how many bytes it spanned,
// or 0 if the sequence is malformed or truncated.
int utf8_decode(const char *p, int len, uint32_t *out);