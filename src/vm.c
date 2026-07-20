#include "vm.h"
#include "chunk.h"
#include "string_utils.h"
#include "value.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#define STACK_MAX 4096
#define FRAMES_MAX 128
#define STACK_HEADROOM 256 // reserve room for one frame's locals/temps

typedef struct {
  FunDef *fun;
  uint8_t *ip;
  Value *base; // first argument / local slot
} Frame;

typedef struct {
  Module *m;

  Value stack[STACK_MAX];
  Value *sp;

  Frame frames[FRAMES_MAX];
  int frame_count;
} Vm;

static bool runtime_error(Vm *vm, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fprintf(stderr, "runtime error: ");
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  va_end(args);

  for (int i = vm->frame_count - 1; i >= 0; i--) {
    FunDef *fun = vm->frames[i].fun;
    fprintf(stderr, "  in " SV_FMT "()\n", SV_ARG(fun->name));
  }
  return false;
}

static inline void push(Vm *vm, Value v) { *vm->sp++ = v; }
static inline Value pop(Vm *vm) { return *--vm->sp; }
static inline Value peek(Vm *vm, int depth) { return vm->sp[-1 - depth]; }

static inline double as_num(Value v) {
  return v.kind == VAL_FLOAT ? v.as.f : (double)v.as.i;
}

static bool value_equal(Value a, Value b) {
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
  }
  return false;
}

bool vm_run(Module *m, FunDef *entry) {
  assert(entry->chunk && "entry function was not compiled");
  if (entry->param_count != 0) {
    fprintf(stderr, "error: '" SV_FMT "' must take no parameters to be run\n",
            SV_ARG(entry->name));
    return false;
  }

  Vm vm = {.m = m, .sp = NULL, .frame_count = 0};
  vm.sp = vm.stack;

  push(&vm, val_fun(entry));
  vm.frames[vm.frame_count++] = (Frame){
      .fun = entry,
      .ip = entry->chunk->code,
      .base = vm.sp,
  };

  Frame *frame = &vm.frames[vm.frame_count - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

  for (;;) {
    OpCode op = (OpCode)READ_BYTE();
    switch (op) {
    case OP_CONST:
      push(&vm, frame->fun->chunk->consts[READ_BYTE()]);
      break;
    case OP_UNIT:
      push(&vm, val_unit());
      break;
    case OP_TRUE:
      push(&vm, val_bool(true));
      break;
    case OP_FALSE:
      push(&vm, val_bool(false));
      break;

    case OP_POP:
      pop(&vm);
      break;
    case OP_POPN:
      vm.sp -= READ_BYTE();
      break;
    case OP_SLIDE: {
      uint8_t n = READ_BYTE();
      Value top = pop(&vm);
      vm.sp -= n;
      push(&vm, top);
      break;
    }

    case OP_GET_LOCAL:
      push(&vm, frame->base[READ_BYTE()]);
      break;
    case OP_SET_LOCAL:
      frame->base[READ_BYTE()] = peek(&vm, 0);
      break;
    case OP_GET_GLOBAL:
      push(&vm, val_fun(vm.m->funs[READ_BYTE()]));
      break;

    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_MOD: {
      Value b = pop(&vm);
      Value a = pop(&vm);
      if (a.kind == VAL_INT && b.kind == VAL_INT) {
        int64_t x = a.as.i, y = b.as.i, r = 0;
        if ((op == OP_DIV || op == OP_MOD) && y == 0) {
          return runtime_error(&vm, "division by zero");
        }
        switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV: r = x / y; break;
        case OP_MOD: r = x % y; break;
        default: break;
        }
        push(&vm, val_int(r));
      } else {
        // Int op Float widens to Float (matches the checker)
        double x = as_num(a), y = as_num(b), r = 0;
        switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV: r = x / y; break;
        case OP_MOD: r = fmod(x, y); break;
        default: break;
        }
        push(&vm, val_float(r));
      }
      break;
    }

    case OP_NEG: {
      Value v = pop(&vm);
      push(&vm, v.kind == VAL_INT ? val_int(-v.as.i) : val_float(-v.as.f));
      break;
    }
    case OP_NOT:
      push(&vm, val_bool(!pop(&vm).as.b));
      break;

    case OP_EQ: {
      Value b = pop(&vm), a = pop(&vm);
      push(&vm, val_bool(value_equal(a, b)));
      break;
    }
    case OP_NEQ: {
      Value b = pop(&vm), a = pop(&vm);
      push(&vm, val_bool(!value_equal(a, b)));
      break;
    }
    case OP_LT:
    case OP_LTEQ:
    case OP_GT:
    case OP_GTEQ: {
      Value b = pop(&vm), a = pop(&vm);
      bool r = false;
      if (a.kind == VAL_INT && b.kind == VAL_INT) {
        switch (op) {
        case OP_LT: r = a.as.i < b.as.i; break;
        case OP_LTEQ: r = a.as.i <= b.as.i; break;
        case OP_GT: r = a.as.i > b.as.i; break;
        case OP_GTEQ: r = a.as.i >= b.as.i; break;
        default: break;
        }
      } else {
        double x = as_num(a), y = as_num(b);
        switch (op) {
        case OP_LT: r = x < y; break;
        case OP_LTEQ: r = x <= y; break;
        case OP_GT: r = x > y; break;
        case OP_GTEQ: r = x >= y; break;
        default: break;
        }
      }
      push(&vm, val_bool(r));
      break;
    }

    case OP_CAST_INT: {
      Value v = pop(&vm);
      push(&vm, v.kind == VAL_FLOAT ? val_int((int64_t)v.as.f) : v);
      break;
    }
    case OP_CAST_FLOAT: {
      Value v = pop(&vm);
      push(&vm, v.kind == VAL_INT ? val_float((double)v.as.i) : v);
      break;
    }

    case OP_RANGE: {
      bool inclusive = READ_BYTE() != 0;
      Value end = pop(&vm), start = pop(&vm);
      Value r = {.kind = VAL_RANGE};
      r.as.range.start = start.as.i;
      r.as.range.end = end.as.i;
      r.as.range.inclusive = inclusive;
      push(&vm, r);
      break;
    }
    case OP_RANGE_START: {
      Value r = pop(&vm);
      push(&vm, val_int(r.as.range.start));
      break;
    }
    case OP_RANGE_TEST: {
      Value i = pop(&vm), r = pop(&vm);
      push(&vm, val_bool(r.as.range.inclusive ? i.as.i <= r.as.range.end
                                              : i.as.i < r.as.range.end));
      break;
    }

    case OP_JUMP: {
      uint16_t dist = READ_U16();
      frame->ip += dist;
      break;
    }
    case OP_JUMP_IF_FALSE: {
      uint16_t dist = READ_U16();
      if (!peek(&vm, 0).as.b) {
        frame->ip += dist;
      }
      break;
    }
    case OP_LOOP: {
      uint16_t dist = READ_U16();
      frame->ip -= dist;
      break;
    }

    case OP_CALL: {
      uint8_t argc = READ_BYTE();
      Value callee = peek(&vm, argc);
      if (callee.kind != VAL_FUN) {
        return runtime_error(&vm, "can only call functions");
      }
      FunDef *fun = callee.as.fun;
      assert(fun->param_count == argc && "arity got past the checker");
      if (fun->chunk == NULL) {
        return runtime_error(&vm, "call to uncompiled function '" SV_FMT "'",
                             SV_ARG(fun->name));
      }
      if (vm.frame_count >= FRAMES_MAX) {
        return runtime_error(&vm, "stack overflow");
      }
      if (vm.sp - vm.stack >= STACK_MAX - STACK_HEADROOM) {
        return runtime_error(&vm, "value stack overflow");
      }
      vm.frames[vm.frame_count++] = (Frame){
          .fun = fun,
          .ip = fun->chunk->code,
          .base = vm.sp - argc,
      };
      frame = &vm.frames[vm.frame_count - 1];
      break;
    }

    case OP_PRINT: {
      value_print(pop(&vm), stdout);
      fputc('\n', stdout);
      push(&vm, val_unit());
      break;
    }

    case OP_RETURN: {
      Value result = pop(&vm);
      vm.sp = frame->base - 1; // also drops the callee value
      vm.frame_count--;
      if (vm.frame_count == 0) {
        return true;
      }
      push(&vm, result);
      frame = &vm.frames[vm.frame_count - 1];
      break;
    }

    default:
      return runtime_error(&vm, "unknown opcode %d", (int)op);
    }
  }

#undef READ_BYTE
#undef READ_U16
}
