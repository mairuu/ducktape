#include "value.h"
#include "ast.h"
#include "object.h"
#include "string_utils.h"

void value_print(Value v, FILE *out) {
  switch (v.kind) {
  case VAL_INT:
    fprintf(out, "%lld", (long long)v.as.i);
    break;
  case VAL_FLOAT:
    fprintf(out, "%g", v.as.f);
    break;
  case VAL_BOOL:
    fprintf(out, v.as.b ? "true" : "false");
    break;
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
    }
    return false;
  }
  }
  return false;
}
