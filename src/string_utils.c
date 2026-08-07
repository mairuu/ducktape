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

int utf8_encode(uint32_t cp, char *out) {
  if (!utf8_is_scalar(cp)) {
    return 0;
  }
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

bool utf8_validate(const char *p, int len) {
  for (int i = 0; i < len;) {
    uint32_t cp;
    int width = utf8_decode(p + i, len - i, &cp);
    if (width == 0) {
      return false;
    }
    i += width;
  }
  return true;
}

int utf8_decode(const char *p, int len, uint32_t *out) {
  if (len <= 0) {
    return 0;
  }
  unsigned char lead = (unsigned char)p[0];
  int width;
  uint32_t cp;
  uint32_t least; // the smallest code point this width may legally encode

  if (lead < 0x80) {
    *out = lead;
    return 1;
  } else if ((lead & 0xE0) == 0xC0) {
    width = 2, cp = lead & 0x1Fu, least = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    width = 3, cp = lead & 0x0Fu, least = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    width = 4, cp = lead & 0x07u, least = 0x10000;
  } else {
    return 0; // a continuation byte where a lead was due, or 0xF8..0xFF
  }

  if (len < width) {
    return 0;
  }
  for (int i = 1; i < width; i++) {
    unsigned char b = (unsigned char)p[i];
    if ((b & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (b & 0x3Fu);
  }

  // an overlong encoding spells a scalar value that a shorter sequence
  // already spells, which would make two different byte strings one char.
  if (cp < least || !utf8_is_scalar(cp)) {
    return 0;
  }
  *out = cp;
  return width;
}