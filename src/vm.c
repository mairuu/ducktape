#include "vm.h"
#include "chunk.h"
#include "object.h"
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
  ObjClosure *closure; // NULL for a plain VAL_FUN call (no upvalues)
  uint8_t *ip;
  Value *base; // first argument / local slot
} Frame;

typedef struct {
  Module *m;
  Heap *heap;

  Value stack[STACK_MAX];
  Value *sp;

  Frame frames[FRAMES_MAX];
  int frame_count;

  ObjUpvalue *open_upvalues; // live upvalues over stack slots, highest first
} Vm;

// find or create the upvalue capturing `slot`. open upvalues are kept in one
// list ordered by descending stack address so a captured slot is shared: two
// closures capturing the same variable get the same ObjUpvalue.
static ObjUpvalue *capture_upvalue(Vm *vm, Value *slot) {
  ObjUpvalue *prev = NULL;
  ObjUpvalue *uv = vm->open_upvalues;
  while (uv != NULL && uv->location > slot) {
    prev = uv;
    uv = uv->next;
  }
  if (uv != NULL && uv->location == slot) {
    return uv;
  }
  ObjUpvalue *created = heap_upvalue(vm->heap, slot);
  created->next = uv;
  if (prev == NULL) {
    vm->open_upvalues = created;
  } else {
    prev->next = created;
  }
  return created;
}

// close (copy the value into the heap cell) every open upvalue at or above
// `last`, since those stack slots are about to die.
static void close_upvalues(Vm *vm, Value *last) {
  while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
    ObjUpvalue *uv = vm->open_upvalues;
    uv->closed = *uv->location;
    uv->location = &uv->closed;
    vm->open_upvalues = uv->next;
  }
}

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

static void vm_mark_roots(void *ctx) {
  Vm *vm = ctx;
  for (Value *v = vm->stack; v < vm->sp; v++) {
    gc_mark_value(*v);
  }
  // open upvalues may be mid-capture (allocated before their closure's array
  // is filled), so root them directly rather than only through their closures.
  for (ObjUpvalue *uv = vm->open_upvalues; uv != NULL; uv = uv->next) {
    gc_mark_value(val_obj((Obj *)uv));
  }
}

// converts a value to its string form for OP_INTERP; strings pass through
// without a new allocation.
static ObjString *stringify(Heap *heap, Value v) {
  if (val_is_string(v)) {
    return val_as_string(v);
  }
  char buf[64];
  int len;
  switch (v.kind) {
  case VAL_INT:
    len = snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i);
    break;
  case VAL_FLOAT:
    len = snprintf(buf, sizeof(buf), "%g", v.as.f);
    break;
  case VAL_BOOL:
    len = snprintf(buf, sizeof(buf), "%s", v.as.b ? "true" : "false");
    break;
  default:
    assert(false && "interpolation segment type got past the checker");
    len = 0;
    break;
  }
  return heap_intern(heap, buf, len);
}

bool vm_run(Module *m, Heap *heap, FunDef *entry) {
  assert(entry->chunk && "entry function was not compiled");
  if (entry->param_count != 0) {
    fprintf(stderr, "error: '" SV_FMT "' must take no parameters to be run\n",
            SV_ARG(entry->name));
    return false;
  }

  Vm vm = {.m = m, .heap = heap, .sp = NULL, .frame_count = 0,
           .open_upvalues = NULL};
  vm.sp = vm.stack;
  heap->mark_roots = vm_mark_roots;
  heap->mark_roots_ctx = &vm;

  push(&vm, val_fun(entry));
  vm.frames[vm.frame_count++] = (Frame){
      .fun = entry,
      .closure = NULL,
      .ip = entry->chunk->code,
      .base = vm.sp,
  };

  Frame *frame = &vm.frames[vm.frame_count - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_U16()                                                             \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define VM_RETURN(x)                                                           \
  do {                                                                         \
    heap->mark_roots = NULL;                                                   \
    return (x);                                                                \
  } while (0)

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
    case OP_GET_GLOBAL: {
      uint8_t slot = READ_BYTE();
      // top-level funs occupy [0, fun_count); impl methods continue the same
      // slot space right after, in [fun_count, fun_count + method_count).
      FunDef *fun = slot < vm.m->fun_count
                       ? vm.m->funs[slot]
                       : vm.m->methods[slot - vm.m->fun_count];
      push(&vm, val_fun(fun));
      break;
    }

    case OP_CLOSURE: {
      uint8_t const_idx = READ_BYTE();
      uint8_t upvalue_count = READ_BYTE();
      FunDef *fun = frame->fun->chunk->consts[const_idx].as.fun;
      ObjClosure *closure = heap_closure(vm.heap, fun, upvalue_count);
      // keep it reachable while capturing upvalues (each capture may allocate)
      push(&vm, val_obj((Obj *)closure));
      for (int i = 0; i < upvalue_count; i++) {
        uint8_t is_local = READ_BYTE();
        uint8_t index = READ_BYTE();
        closure->upvalues[i] = is_local
                                   ? capture_upvalue(&vm, frame->base + index)
                                   : frame->closure->upvalues[index];
      }
      break;
    }
    case OP_GET_UPVALUE: {
      uint8_t idx = READ_BYTE();
      push(&vm, *frame->closure->upvalues[idx]->location);
      break;
    }
    case OP_SET_UPVALUE: {
      uint8_t idx = READ_BYTE();
      *frame->closure->upvalues[idx]->location = peek(&vm, 0);
      break;
    }
    case OP_CLOSE_UPVALUE:
      close_upvalues(&vm, frame->base + READ_BYTE());
      break;

    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_MOD: {
      // peek (not pop): a heap_concat below may collect, and the operands
      // must still be reachable from the VM stack when it does
      Value b = peek(&vm, 0);
      Value a = peek(&vm, 1);
      Value result;
      if (op == OP_ADD && val_is_string(a) && val_is_string(b)) {
        result = val_obj(
            &heap_concat(vm.heap, val_as_string(a), val_as_string(b))->obj);
      } else if (a.kind == VAL_INT && b.kind == VAL_INT) {
        int64_t x = a.as.i, y = b.as.i, r = 0;
        if ((op == OP_DIV || op == OP_MOD) && y == 0) {
          VM_RETURN(runtime_error(&vm, "division by zero"));
        }
        switch (op) {
        case OP_ADD:
          r = x + y;
          break;
        case OP_SUB:
          r = x - y;
          break;
        case OP_MUL:
          r = x * y;
          break;
        case OP_DIV:
          r = x / y;
          break;
        case OP_MOD:
          r = x % y;
          break;
        default:
          break;
        }
        result = val_int(r);
      } else {
        // Int op Float widens to Float (matches the checker)
        double x = as_num(a), y = as_num(b), r = 0;
        switch (op) {
        case OP_ADD:
          r = x + y;
          break;
        case OP_SUB:
          r = x - y;
          break;
        case OP_MUL:
          r = x * y;
          break;
        case OP_DIV:
          r = x / y;
          break;
        case OP_MOD:
          r = fmod(x, y);
          break;
        default:
          break;
        }
        result = val_float(r);
      }
      vm.sp -= 2;
      push(&vm, result);
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
        case OP_LT:
          r = a.as.i < b.as.i;
          break;
        case OP_LTEQ:
          r = a.as.i <= b.as.i;
          break;
        case OP_GT:
          r = a.as.i > b.as.i;
          break;
        case OP_GTEQ:
          r = a.as.i >= b.as.i;
          break;
        default:
          break;
        }
      } else {
        double x = as_num(a), y = as_num(b);
        switch (op) {
        case OP_LT:
          r = x < y;
          break;
        case OP_LTEQ:
          r = x <= y;
          break;
        case OP_GT:
          r = x > y;
          break;
        case OP_GTEQ:
          r = x >= y;
          break;
        default:
          break;
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
      FunDef *fun;
      ObjClosure *closure = NULL;
      if (callee.kind == VAL_FUN) {
        fun = callee.as.fun;
      } else if (val_is_closure(callee)) {
        closure = val_as_closure(callee);
        fun = closure->fun;
      } else {
        VM_RETURN(runtime_error(&vm, "can only call functions"));
      }
      assert(fun->param_count == argc && "arity got past the checker");
      if (fun->chunk == NULL) {
        VM_RETURN(runtime_error(&vm, "call to uncompiled function '" SV_FMT "'",
                                SV_ARG(fun->name)));
      }
      if (vm.frame_count >= FRAMES_MAX) {
        VM_RETURN(runtime_error(&vm, "stack overflow"));
      }
      if (vm.sp - vm.stack >= STACK_MAX - STACK_HEADROOM) {
        VM_RETURN(runtime_error(&vm, "value stack overflow"));
      }
      vm.frames[vm.frame_count++] = (Frame){
          .fun = fun,
          .closure = closure,
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
      // this frame's locals/args are dying; close any upvalues over them so
      // escaping closures keep their own copies.
      close_upvalues(&vm, frame->base);
      vm.sp = frame->base - 1; // also drops the callee value
      vm.frame_count--;
      if (vm.frame_count == 0) {
        VM_RETURN(true);
      }
      push(&vm, result);
      frame = &vm.frames[vm.frame_count - 1];
      break;
    }

    case OP_ARRAY: {
      uint8_t count = READ_BYTE();
      Value *elems = vm.sp - count; // still-live roots until copied
      ObjArray *arr = heap_array(vm.heap, count);
      for (int i = 0; i < count; i++) {
        arr->items[i] = elems[i];
      }
      vm.sp -= count;
      push(&vm, val_obj(&arr->obj));
      break;
    }

    case OP_INDEX_GET: {
      Value idxv = pop(&vm), arrv = pop(&vm);
      ObjArray *arr = val_as_array(arrv);
      int64_t idx = idxv.as.i;
      if (idx < 0 || idx >= arr->count) {
        VM_RETURN(runtime_error(&vm, "index %lld out of bounds (len %d)",
                                (long long)idx, arr->count));
      }
      push(&vm, arr->items[idx]);
      break;
    }
    case OP_INDEX_SET: {
      Value val = pop(&vm), idxv = pop(&vm), arrv = pop(&vm);
      ObjArray *arr = val_as_array(arrv);
      int64_t idx = idxv.as.i;
      if (idx < 0 || idx >= arr->count) {
        VM_RETURN(runtime_error(&vm, "index %lld out of bounds (len %d)",
                                (long long)idx, arr->count));
      }
      arr->items[idx] = val;
      push(&vm, val);
      break;
    }
    case OP_LEN: {
      ObjArray *arr = val_as_array(pop(&vm));
      push(&vm, val_int(arr->count));
      break;
    }

    case OP_INTERP: {
      uint8_t n = READ_BYTE();
      Value *segs = vm.sp - n; // still-live roots throughout
      ObjString *acc = stringify(vm.heap, segs[0]);
      push(&vm, val_obj(&acc->obj)); // keep the accumulator rooted
      for (int i = 1; i < n; i++) {
        ObjString *piece = stringify(vm.heap, segs[i]);
        acc = heap_concat(vm.heap, acc, piece);
        vm.sp[-1] = val_obj(&acc->obj);
      }
      Value result = pop(&vm);
      vm.sp -= n;
      push(&vm, result);
      break;
    }

    case OP_DUP:
      push(&vm, peek(&vm, READ_BYTE()));
      break;

    case OP_TUPLE: {
      uint8_t count = READ_BYTE();
      Value *elems = vm.sp - count; // still-live roots until copied
      ObjTuple *tup = heap_tuple(vm.heap, count);
      for (int i = 0; i < count; i++) {
        tup->items[i] = elems[i];
      }
      vm.sp -= count;
      push(&vm, val_obj(&tup->obj));
      break;
    }

    case OP_STRUCT: {
      StructDef *def = vm.m->structs[READ_BYTE()];
      Value *elems = vm.sp - def->field_count; // still-live roots
      ObjStruct *s = heap_struct(vm.heap, def);
      for (int i = 0; i < def->field_count; i++) {
        s->fields[i] = elems[i];
      }
      vm.sp -= def->field_count;
      push(&vm, val_obj(&s->obj));
      break;
    }

    case OP_ENUM: {
      uint8_t enum_slot = READ_BYTE();
      uint8_t tag = READ_BYTE();
      VariantDef *variant = &vm.m->enums[enum_slot]->variants[tag];
      Value *elems = vm.sp - variant->field_count; // still-live roots
      ObjEnum *e = heap_enum(vm.heap, variant);
      for (int i = 0; i < variant->field_count; i++) {
        e->fields[i] = elems[i];
      }
      vm.sp -= variant->field_count;
      push(&vm, val_obj(&e->obj));
      break;
    }

    case OP_FIELD_GET: {
      uint8_t idx = READ_BYTE();
      Value v = pop(&vm);
      Value *fields;
      switch (v.as.obj->kind) {
      case OBJ_TUPLE:
        fields = val_as_tuple(v)->items;
        break;
      case OBJ_STRUCT:
        fields = val_as_struct(v)->fields;
        break;
      case OBJ_ENUM:
        fields = val_as_enum(v)->fields;
        break;
      default:
        assert(false && "OP_FIELD_GET on a non-aggregate value");
        fields = NULL;
      }
      push(&vm, fields[idx]);
      break;
    }

    case OP_TAG:
      push(&vm, val_int(val_as_enum(pop(&vm))->variant->tag));
      break;

    case OP_MATCH_FAIL:
      // the checker doesn't enforce match exhaustiveness yet, so this is a
      // real, reachable failure mode rather than a should-never-happen case.
      VM_RETURN(runtime_error(&vm, "no match arm matched"));

    default:
      VM_RETURN(runtime_error(&vm, "unknown opcode %d", (int)op));
    }
  }

#undef READ_BYTE
#undef READ_U16
#undef VM_RETURN
}
