#include "value.h"
#include "ast.h"
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
  }
}
