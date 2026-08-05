#include "value.h"
#include "ast.h"
#include "object.h"
#include "string_utils.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int value_format_float(double f, char *buf, size_t cap) {
  if (isnan(f)) {
    return snprintf(buf, cap, "NaN");
  }
  if (isinf(f)) {
    return snprintf(buf, cap, f < 0 ? "-inf" : "inf");
  }

  // the fewest significant digits that still read back as the same double:
  // fewer would lose the value, more would print noise the user never typed.
  // 17 always suffice for a binary64.
  char sci[32];
  int digits = 17;
  for (int d = 1; d <= 17; d++) {
    snprintf(sci, sizeof(sci), "%.*e", d - 1, f);
    if (strtod(sci, NULL) == f) {
      digits = d;
      break;
    }
  }
  int exp10 = (int)strtol(strchr(sci, 'e') + 1, NULL, 10);

  int len;
  if (exp10 < -5 || exp10 >= 17) {
    // far enough out that the digits would be mostly zeros
    len = snprintf(buf, cap, "%.*e", digits - 1, f);
  } else {
    // plain notation in the range a reader can still count: `300.0`, not the
    // `3e+02` that picking the shortest `%g` would give.
    int decimals = digits - 1 - exp10;
    len = snprintf(buf, cap, "%.*f", decimals < 0 ? 0 : decimals, f);
  }

  // `%g` drops the point on a whole value, which would make `1.0` print as
  // `1` — indistinguishable from the int.
  if (!strpbrk(buf, ".e") && (size_t)len + 3 <= cap) {
    len += snprintf(buf + len, cap - (size_t)len, ".0");
  }
  return len;
}

void value_print(Value v, FILE *out) {
  switch (v.kind) {
  case VAL_INT:
    fprintf(out, "%lld", (long long)v.as.i);
    break;
  case VAL_FLOAT: {
    char buf[32];
    value_format_float(v.as.f, buf, sizeof(buf));
    fputs(buf, out);
    break;
  }
  case VAL_BOOL:
    fprintf(out, v.as.b ? "true" : "false");
    break;
  case VAL_CHAR: {
    // undecorated, like the String below: `print` shows the character, not
    // the literal that spells it. A char is always a scalar value, so the
    // encode cannot fail.
    char buf[UTF8_MAX_BYTES];
    int n = utf8_encode(v.as.c, buf);
    fwrite(buf, 1, (size_t)n, out);
    break;
  }
  case VAL_UNIT:
    fprintf(out, "()");
    break;
  case VAL_RANGE:
    fprintf(out, "%lld..%s%lld", (long long)v.as.range.start,
            v.as.range.inclusive ? "=" : "", (long long)v.as.range.end);
    break;
  case VAL_FUN:
    fprintf(out, "<fun " SV_FMT ">", SV_ARG(v.as.fun->name));
    break;
  case VAL_OBJ:
    switch (v.as.obj->kind) {
    case OBJ_STRING:
      fprintf(out, "%s", val_as_string(v)->chars);
      break;
    case OBJ_STRBUF: {
      // decorated, unlike the String above it: the whole of a StringBuf is
      // that it is *not* one yet, so the debug view says which it is looking
      // at. The bytes are not NUL-terminated — only `build` produces that.
      ObjStrBuf *buf = val_as_strbuf(v);
      fprintf(out, "StringBuf(\"%.*s\")", buf->len,
              buf->bytes != NULL ? buf->bytes : "");
      break;
    }
    case OBJ_ARRAY: {
      ObjArray *arr = val_as_array(v);
      fputc('[', out);
      for (int i = 0; i < arr->count; i++) {
        if (i > 0) {
          fprintf(out, ", ");
        }
        value_print(arr->items[i], out);
      }
      fputc(']', out);
      break;
    }
    case OBJ_TUPLE: {
      ObjTuple *tup = val_as_tuple(v);
      fputc('(', out);
      for (int i = 0; i < tup->count; i++) {
        if (i > 0) {
          fprintf(out, ", ");
        }
        value_print(tup->items[i], out);
      }
      fputc(')', out);
      break;
    }
    case OBJ_STRUCT: {
      ObjStruct *s = val_as_struct(v);
      StructDef *def = s->def;
      fprintf(out, SV_FMT, SV_ARG(def->name));
      if (def->field_count == 0) {
        break;
      }
      fprintf(out, def->is_tuple ? "(" : " { ");
      for (int i = 0; i < def->field_count; i++) {
        if (i > 0) {
          fprintf(out, ", ");
        }
        if (!def->is_tuple) {
          fprintf(out, SV_FMT ": ", SV_ARG(def->fields[i].ident.name));
        }
        value_print(s->fields[i], out);
      }
      fprintf(out, def->is_tuple ? ")" : " }");
      break;
    }
    case OBJ_ENUM: {
      ObjEnum *e = val_as_enum(v);
      VariantDef *variant = e->variant;
      fprintf(out, SV_FMT, SV_ARG(variant->name));
      if (variant->field_count == 0) {
        break;
      }
      fprintf(out, variant->is_tuple ? "(" : " { ");
      for (int i = 0; i < variant->field_count; i++) {
        if (i > 0) {
          fprintf(out, ", ");
        }
        if (!variant->is_tuple) {
          fprintf(out, SV_FMT ": ", SV_ARG(variant->fields[i].ident.name));
        }
        value_print(e->fields[i], out);
      }
      fprintf(out, variant->is_tuple ? ")" : " }");
      break;
    }
    case OBJ_CLOSURE:
      fprintf(out, "<fun " SV_FMT ">", SV_ARG(val_as_closure(v)->fun->name));
      break;
    case OBJ_UPVALUE:
      // never a first-class value the language can name or print
      fprintf(out, "<upvalue>");
      break;
    case OBJ_DYN:
      // the wrapper is an implementation detail of dispatch, not something the
      // program put in the value — so it prints as whatever it wraps.
      value_print(val_as_dyn(v)->inner, out);
      break;
    }
    break;
  }
}

bool value_equal(Value a, Value b) {
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
  case VAL_INT:
    return a.as.i == b.as.i;
  case VAL_FLOAT:
    return a.as.f == b.as.f;
  case VAL_BOOL:
    return a.as.b == b.as.b;
  case VAL_CHAR:
    return a.as.c == b.as.c;
  case VAL_UNIT:
    return true;
  case VAL_RANGE:
    return a.as.range.start == b.as.range.start &&
           a.as.range.end == b.as.range.end &&
           a.as.range.inclusive == b.as.range.inclusive;
  case VAL_FUN:
    return a.as.fun == b.as.fun;
  case VAL_OBJ: {
    if (a.as.obj->kind != b.as.obj->kind) {
      return false;
    }
    switch (a.as.obj->kind) {
    case OBJ_STRING:
      return a.as.obj == b.as.obj; // interned
    case OBJ_STRBUF: {
      // *not* interned, so this is the one place the two kinds visibly differ:
      // a String compares by pointer, a buffer has to compare its bytes.
      ObjStrBuf *x = val_as_strbuf(a), *y = val_as_strbuf(b);
      // an empty buffer has no allocation at all, so the length test comes
      // first: memcmp is not defined on a NULL pointer even for zero bytes.
      return x->len == y->len &&
             (x->len == 0 || memcmp(x->bytes, y->bytes, (size_t)x->len) == 0);
    }
    case OBJ_ARRAY: {
      ObjArray *x = val_as_array(a), *y = val_as_array(b);
      if (x->count != y->count) {
        return false;
      }
      for (int i = 0; i < x->count; i++) {
        if (!value_equal(x->items[i], y->items[i])) {
          return false;
        }
      }
      return true;
    }
    case OBJ_TUPLE: {
      ObjTuple *x = val_as_tuple(a), *y = val_as_tuple(b);
      for (int i = 0; i < x->count; i++) {
        if (!value_equal(x->items[i], y->items[i])) {
          return false;
        }
      }
      return true;
    }
    case OBJ_STRUCT: {
      ObjStruct *x = val_as_struct(a), *y = val_as_struct(b);
      for (int i = 0; i < x->def->field_count; i++) {
        if (!value_equal(x->fields[i], y->fields[i])) {
          return false;
        }
      }
      return true;
    }
    case OBJ_ENUM: {
      ObjEnum *x = val_as_enum(a), *y = val_as_enum(b);
      if (x->variant != y->variant) {
        return false;
      }
      for (int i = 0; i < x->variant->field_count; i++) {
        if (!value_equal(x->fields[i], y->fields[i])) {
          return false;
        }
      }
      return true;
    }
    case OBJ_DYN:
      // compare through the wrapper, matching how it prints. Two trait objects
      // over different concrete types are still unequal — the inner values
      // have different kinds (or defs), which the recursion catches.
      return value_equal(val_as_dyn(a)->inner, val_as_dyn(b)->inner);
    case OBJ_CLOSURE:
    case OBJ_UPVALUE:
      return a.as.obj == b.as.obj; // identity; not structurally comparable
    }
    return false;
  }
  }
  return false;
}
