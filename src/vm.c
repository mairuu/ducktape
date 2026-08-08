#include "vm.h"
#include "chunk.h"
#include "object.h"
#include "string_utils.h"
#include "value.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
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

struct Vm {
  Executable *exe;
  Heap *heap;

  Value stack[STACK_MAX];
  Value *sp;

  Frame frames[FRAMES_MAX];
  int frame_count;

  ObjUpvalue *open_upvalues; // live upvalues over stack slots, highest first
};

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
    len = value_format_float(v.as.f, buf, sizeof(buf));
    break;
  case VAL_BOOL:
    len = snprintf(buf, sizeof(buf), "%s", v.as.b ? "true" : "false");
    break;
  case VAL_CHAR:
    len = utf8_encode(v.as.c, buf);
    break;
  default:
    assert(false && "interpolation segment type got past the checker");
    len = 0;
    break;
  }
  return heap_intern(heap, buf, len);
}

// Drive bytecode until the frame that was on top when this call began has
// returned — `stop_depth` is `frame_count` from *before* that frame was pushed,
// so the loop ends the moment the count falls back to it, leaving the returned
// value on the stack.
//
// The parameter is the whole of what makes the interpreter **re-entrant**, and
// re-entrancy is what lets a native call back into ducktape (`native_call`
// below): the outer `run` is still suspended in C, its frames untouched
// underneath, while the inner one drives the callee to its `OP_RETURN`. At the
// top level `stop_depth` is 0 and this is the loop it has always been.
//
// A re-entrant interpreter is a re-entrant *collector* too: user code called
// from C allocates, so a collection can now happen at any point inside a
// native. Everything a native holds across such a call must therefore be a
// root — which for its arguments it already is, and for anything else is what
// `native_root` is for.
static bool run(Vm *vm, int stop_depth) {
  Frame *frame = &vm->frames[vm->frame_count - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_U16()                                                             \
  (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define VM_RETURN(x) return (x)

  for (;;) {
    OpCode op = (OpCode)READ_BYTE();
    switch (op) {
    case OP_CONST:
      push(vm, frame->fun->chunk->consts[READ_U16()]);
      break;
    case OP_UNIT:
      push(vm, val_unit());
      break;
    case OP_TRUE:
      push(vm, val_bool(true));
      break;
    case OP_FALSE:
      push(vm, val_bool(false));
      break;

    case OP_POP:
      pop(vm);
      break;
    case OP_POPN:
      vm->sp -= READ_BYTE();
      break;
    case OP_SLIDE: {
      uint8_t n = READ_BYTE();
      Value top = pop(vm);
      vm->sp -= n;
      push(vm, top);
      break;
    }

    case OP_GET_LOCAL:
      push(vm, frame->base[READ_BYTE()]);
      break;
    case OP_SET_LOCAL:
      frame->base[READ_BYTE()] = peek(vm, 0);
      break;
    case OP_GET_GLOBAL: {
      // one program-wide slot space over every module's funs and impl
      // methods, handed out on demand by codegen.
      push(vm, val_fun(vm->exe->globals[READ_U16()]));
      break;
    }

    case OP_CLOSURE: {
      uint16_t const_idx = READ_U16();
      uint8_t upvalue_count = READ_BYTE();
      FunDef *fun = frame->fun->chunk->consts[const_idx].as.fun;
      ObjClosure *closure = heap_closure(vm->heap, fun, upvalue_count);
      // keep it reachable while capturing upvalues (each capture may allocate)
      push(vm, val_obj((Obj *)closure));
      for (int i = 0; i < upvalue_count; i++) {
        uint8_t is_local = READ_BYTE();
        uint8_t index = READ_BYTE();
        closure->upvalues[i] = is_local
                                   ? capture_upvalue(vm, frame->base + index)
                                   : frame->closure->upvalues[index];
      }
      break;
    }
    case OP_GET_UPVALUE: {
      uint8_t idx = READ_BYTE();
      push(vm, *frame->closure->upvalues[idx]->location);
      break;
    }
    case OP_SET_UPVALUE: {
      uint8_t idx = READ_BYTE();
      *frame->closure->upvalues[idx]->location = peek(vm, 0);
      break;
    }
    case OP_CLOSE_UPVALUE:
      close_upvalues(vm, frame->base + READ_BYTE());
      break;

    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_MOD: {
      // peek (not pop): a heap_concat below may collect, and the operands
      // must still be reachable from the VM stack when it does
      Value b = peek(vm, 0);
      Value a = peek(vm, 1);
      Value result;
      if (op == OP_ADD && val_is_string(a) && val_is_string(b)) {
        result = val_obj(
            &heap_concat(vm->heap, val_as_string(a), val_as_string(b))->obj);
      } else if (a.kind == VAL_INT && b.kind == VAL_INT) {
        int64_t x = a.as.i, y = b.as.i, r = 0;
        if ((op == OP_DIV || op == OP_MOD) && y == 0) {
          VM_RETURN(runtime_error(vm, "division by zero"));
        }
        // The language promises that `int` **wraps**, two's complement. Doing
        // that with signed operands is undefined behaviour in C — it happens
        // to produce the promised answer on the hardware, but a sanitizer
        // reports it and an optimiser is entitled to assume it cannot happen.
        // Computing in `uint64_t` and converting back is the same instruction
        // with the promise actually written down: unsigned arithmetic is
        // defined to wrap, and the narrowing conversion is two's complement in
        // C23. (Found by UBSan the moment a hash mixer was written in
        // ducktape; every multiply in one overflows by design.)
        uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
        switch (op) {
        case OP_ADD:
          r = (int64_t)(ux + uy);
          break;
        case OP_SUB:
          r = (int64_t)(ux - uy);
          break;
        case OP_MUL:
          r = (int64_t)(ux * uy);
          break;
        // `INT64_MIN / -1` is the one division that overflows, and unlike the
        // rest it is not merely undefined — on x86 it raises SIGFPE and kills
        // the process, so a ducktape program could crash the VM outright. The
        // wrapping answer is `INT64_MIN` again (`+2^63` has no representation
        // and folds back), and the remainder is exactly zero.
        case OP_DIV:
          r = (x == INT64_MIN && y == -1) ? INT64_MIN : x / y;
          break;
        case OP_MOD:
          r = (x == INT64_MIN && y == -1) ? 0 : x % y;
          break;
        default:
          break;
        }
        result = val_int(r);
      } else {
        // int op float widens to float (matches the checker)
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
      vm->sp -= 2;
      push(vm, result);
      break;
    }

    // The bitwise operators are int-only — the checker has already refused a
    // float or anything else — so unlike the arithmetic above there is no
    // widening case and no string case to test for.
    case OP_BIT_AND:
    case OP_BIT_OR:
    case OP_BIT_XOR: {
      Value b = pop(vm), a = pop(vm);
      uint64_t x = (uint64_t)a.as.i, y = (uint64_t)b.as.i;
      uint64_t r = op == OP_BIT_AND  ? (x & y)
                   : op == OP_BIT_OR ? (x | y)
                                     : (x ^ y);
      push(vm, val_int((int64_t)r));
      break;
    }

    case OP_BIT_NOT:
      push(vm, val_int((int64_t)~(uint64_t)pop(vm).as.i));
      break;

    // A shift count outside 0..63 is **a runtime error, not a wrapped count**.
    // C leaves it undefined and Java quietly masks it to the low six bits, so
    // `x << 64` there is `x` — an answer that looks deliberate and is almost
    // never meant. An array index out of range already reports here rather
    // than guessing, and a shift is the same kind of mistake.
    case OP_SHL:
    case OP_SHR:
    case OP_USHR: {
      Value b = pop(vm), a = pop(vm);
      int64_t n = b.as.i;
      if (n < 0 || n > 63) {
        VM_RETURN(runtime_error(vm, "shift count must be between 0 and 63"));
      }
      uint64_t x = (uint64_t)a.as.i;
      int64_t r;
      if (op == OP_SHL) {
        // shifting in the unsigned domain: a signed left shift that pushes
        // bits past the sign is undefined in C, and the language promises
        // wrapping
        r = (int64_t)(x << n);
      } else if (op == OP_USHR) {
        r = (int64_t)(x >> n);
      } else {
        // arithmetic: propagate the sign rather than the zero, so `>>` stays
        // division by a power of two on a negative
        r = a.as.i >> n;
      }
      push(vm, val_int(r));
      break;
    }

    case OP_NEG: {
      Value v = pop(vm);
      push(vm, v.kind == VAL_INT ? val_int(-v.as.i) : val_float(-v.as.f));
      break;
    }
    case OP_NOT:
      push(vm, val_bool(!pop(vm).as.b));
      break;

    case OP_EQ: {
      Value b = pop(vm), a = pop(vm);
      push(vm, val_bool(value_equal(a, b)));
      break;
    }
    case OP_NEQ: {
      Value b = pop(vm), a = pop(vm);
      push(vm, val_bool(!value_equal(a, b)));
      break;
    }
    case OP_LT:
    case OP_LTEQ:
    case OP_GT:
    case OP_GTEQ: {
      Value b = pop(vm), a = pop(vm);
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
      push(vm, val_bool(r));
      break;
    }

    case OP_CAST_INT: {
      Value v = pop(vm);
      push(vm, v.kind == VAL_FLOAT ? val_int((int64_t)v.as.f) : v);
      break;
    }
    case OP_CAST_FLOAT: {
      Value v = pop(vm);
      push(vm, v.kind == VAL_INT ? val_float((double)v.as.i) : v);
      break;
    }

    case OP_RANGE: {
      bool inclusive = READ_BYTE() != 0;
      Value end = pop(vm), start = pop(vm);
      Value r = {.kind = VAL_RANGE};
      r.as.range.start = start.as.i;
      // saturate rather than wrap: `a..=INT64_MAX` loses its last element
      // instead of becoming an empty range.
      r.as.range.end =
          inclusive && end.as.i != INT64_MAX ? end.as.i + 1 : end.as.i;
      push(vm, r);
      break;
    }
    case OP_RANGE_START: {
      Value r = pop(vm);
      push(vm, val_int(r.as.range.start));
      break;
    }
    case OP_RANGE_TEST: {
      Value i = pop(vm), r = pop(vm);
      push(vm, val_bool(i.as.i < r.as.range.end));
      break;
    }
    case OP_RANGE_STOP: {
      Value r = pop(vm);
      push(vm, val_int(r.as.range.end));
      break;
    }

    case OP_JUMP: {
      uint16_t dist = READ_U16();
      frame->ip += dist;
      break;
    }
    case OP_JUMP_IF_FALSE: {
      uint16_t dist = READ_U16();
      if (!peek(vm, 0).as.b) {
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
      Value callee = peek(vm, argc);
      FunDef *fun;
      ObjClosure *closure = NULL;
      if (callee.kind == VAL_FUN) {
        fun = callee.as.fun;
      } else if (val_is_closure(callee)) {
        closure = val_as_closure(callee);
        fun = closure->fun;
      } else {
        VM_RETURN(runtime_error(vm, "can only call functions"));
      }
      assert(fun->param_count == argc && "arity got past the checker");

      if (fun->native != NULL) {
        // no frame: the C function runs to completion in place. Its arguments
        // stay on the stack across the call — they are the root set, so an
        // allocating native cannot have them collected out from under it —
        // and only afterwards are they replaced by the result. Same
        // discipline OP_MAKE_DYN follows for heap_dyn.
        //
        // `vm` in the context is what lets the native run ducktape of its own
        // (`native_call`). Such a native may use the stack *above* its
        // arguments as scratch roots, and must leave it as it found it — the
        // assert is that contract, since a native that returned with the stack
        // askew would corrupt the frame beneath it rather than fail.
        NativeCtx ctx = {.heap = vm->heap, .error = NULL, .vm = vm};
        Value *sp_before = vm->sp;
        Value result = fun->native(&ctx, vm->sp - argc, argc);
        if (ctx.unwinding) {
          // a call back into ducktape failed and has already reported itself,
          // frames and all; adding a second message would describe the same
          // failure from the outside.
          VM_RETURN(false);
        }
        if (ctx.error != NULL) {
          VM_RETURN(runtime_error(vm, "%s", ctx.error));
        }
        assert(vm->sp == sp_before && "a native must restore the value stack");
        (void)sp_before;
        vm->sp -= argc + 1; // the args, and the callee beneath them
        push(vm, result);
        break;
      }

      if (fun->chunk == NULL) {
        VM_RETURN(runtime_error(vm, "call to uncompiled function '" SV_FMT "'",
                                SV_ARG(fun->name)));
      }
      if (vm->frame_count >= FRAMES_MAX) {
        VM_RETURN(runtime_error(vm, "stack overflow"));
      }
      if (vm->sp - vm->stack >= STACK_MAX - STACK_HEADROOM) {
        VM_RETURN(runtime_error(vm, "value stack overflow"));
      }
      vm->frames[vm->frame_count++] = (Frame){
          .fun = fun,
          .closure = closure,
          .ip = fun->chunk->code,
          .base = vm->sp - argc,
      };
      frame = &vm->frames[vm->frame_count - 1];
      break;
    }

    case OP_RETURN: {
      Value result = pop(vm);
      // this frame's locals/args are dying; close any upvalues over them so
      // escaping closures keep their own copies.
      close_upvalues(vm, frame->base);
      vm->sp = frame->base - 1; // also drops the callee value
      vm->frame_count--;
      // the result replaces callee-and-arguments whoever is waiting for it:
      // the caller's next opcode, or the C that re-entered here and pops it.
      push(vm, result);
      if (vm->frame_count == stop_depth) {
        VM_RETURN(true);
      }
      frame = &vm->frames[vm->frame_count - 1];
      break;
    }

    case OP_ARRAY: {
      uint8_t count = READ_BYTE();
      Value *elems = vm->sp - count; // still-live roots until copied
      ObjArray *arr = heap_array(vm->heap, count);
      for (int i = 0; i < count; i++) {
        arr->items[i] = elems[i];
      }
      vm->sp -= count;
      push(vm, val_obj(&arr->obj));
      break;
    }

    case OP_INDEX_GET: {
      Value idxv = pop(vm), arrv = pop(vm);
      ObjArray *arr = val_as_array(arrv);
      int64_t idx = idxv.as.i;
      if (idx < 0 || idx >= arr->count) {
        VM_RETURN(runtime_error(vm, "index %lld out of bounds (len %d)",
                                (long long)idx, arr->count));
      }
      push(vm, arr->items[idx]);
      break;
    }
    case OP_INDEX_SET: {
      Value val = pop(vm), idxv = pop(vm), arrv = pop(vm);
      ObjArray *arr = val_as_array(arrv);
      int64_t idx = idxv.as.i;
      if (idx < 0 || idx >= arr->count) {
        VM_RETURN(runtime_error(vm, "index %lld out of bounds (len %d)",
                                (long long)idx, arr->count));
      }
      arr->items[idx] = val;
      push(vm, val);
      break;
    }
    case OP_LEN: {
      ObjArray *arr = val_as_array(pop(vm));
      push(vm, val_int(arr->count));
      break;
    }

    case OP_INTERP: {
      uint8_t n = READ_BYTE();
      Value *segs = vm->sp - n; // still-live roots throughout
      ObjString *acc = stringify(vm->heap, segs[0]);
      push(vm, val_obj(&acc->obj)); // keep the accumulator rooted
      for (int i = 1; i < n; i++) {
        ObjString *piece = stringify(vm->heap, segs[i]);
        acc = heap_concat(vm->heap, acc, piece);
        vm->sp[-1] = val_obj(&acc->obj);
      }
      Value result = pop(vm);
      vm->sp -= n;
      push(vm, result);
      break;
    }

    case OP_DUP:
      push(vm, peek(vm, READ_BYTE()));
      break;

    case OP_TUPLE: {
      uint8_t count = READ_BYTE();
      Value *elems = vm->sp - count; // still-live roots until copied
      ObjTuple *tup = heap_tuple(vm->heap, count);
      for (int i = 0; i < count; i++) {
        tup->items[i] = elems[i];
      }
      vm->sp -= count;
      push(vm, val_obj(&tup->obj));
      break;
    }

    case OP_STRUCT: {
      StructDef *def = vm->exe->structs[READ_U16()];
      Value *elems = vm->sp - def->field_count; // still-live roots
      ObjStruct *s = heap_struct(vm->heap, def);
      for (int i = 0; i < def->field_count; i++) {
        s->fields[i] = elems[i];
      }
      vm->sp -= def->field_count;
      push(vm, val_obj(&s->obj));
      break;
    }

    case OP_ENUM: {
      uint16_t enum_slot = READ_U16();
      uint8_t tag = READ_BYTE();
      VariantDef *variant = &vm->exe->enums[enum_slot]->variants[tag];
      // one word-sized field means no object at all — the whole instance fits
      // in the Value. See `val_variant`; this is what makes `Some(x)` free.
      if (variant->field_count == 1 && val_variant_fits(peek(vm, 0))) {
        Value field = pop(vm);
        push(vm, val_variant(variant, field));
        break;
      }
      Value *elems = vm->sp - variant->field_count; // still-live roots
      ObjEnum *e = heap_enum(vm->heap, variant);
      for (int i = 0; i < variant->field_count; i++) {
        e->fields[i] = elems[i];
      }
      vm->sp -= variant->field_count;
      push(vm, val_obj(&e->obj));
      break;
    }

    case OP_FIELD_GET: {
      uint8_t idx = READ_BYTE();
      Value v = pop(vm);
      if (v.kind == VAL_VARIANT) {
        assert(idx == 0 && "an inline variant has exactly one field");
        push(vm, val_variant_field(v));
        break;
      }
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
      push(vm, fields[idx]);
      break;
    }

    case OP_FIELD_SET: {
      uint8_t idx = READ_BYTE();
      Value value = pop(vm);
      Value v = pop(vm);
      // an inline variant has no slot to write: assignment needs a place, and
      // the checker refuses field access on an enum, so no source reaches here.
      assert(v.kind == VAL_OBJ && "OP_FIELD_SET on a value with no fields");
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
        assert(false && "OP_FIELD_SET on a non-aggregate value");
        fields = NULL;
      }
      fields[idx] = value;
      push(vm, value); // assignment is an expression
      break;
    }

    case OP_TAG: {
      Value v = pop(vm);
      VariantDef *variant =
          v.kind == VAL_VARIANT ? val_variant_def(v) : val_as_enum(v)->variant;
      push(vm, val_int(variant->tag));
      break;
    }

    case OP_MAKE_DYN: {
      VTable *vt = vm->exe->vtables[READ_U16()];
      // the operand stays on the stack across heap_dyn: allocating may
      // collect, and until the wrapper exists the stack is what keeps the
      // value alive.
      ObjDyn *d = heap_dyn(vm->heap, peek(vm, 0), vt);
      pop(vm);
      push(vm, val_obj((Obj *)d));
      break;
    }

    case OP_DYN_METHOD: {
      uint8_t index = READ_BYTE();
      ObjDyn *d = val_as_dyn(pop(vm));
      assert(index < d->vtable->method_count && "vtable index out of range");
      // leave the call in the shape OP_CALL already understands: callee
      // beneath its arguments, with the *unwrapped* receiver as argument
      // zero — the method was compiled for the concrete type, not for the
      // trait object.
      push(vm, val_fun(d->vtable->methods[index]));
      push(vm, d->inner);
      break;
    }

    case OP_DYN_IS: {
      VTable *vt = vm->exe->vtables[READ_U16()];
      // pointer equality *is* the type test: `cg_vtable_for` hands out one
      // table per (trait reference, concrete type), so two objects share a
      // table exactly when they were coerced from the same type.
      push(vm, val_bool(val_as_dyn(peek(vm, 0))->vtable == vt));
      break;
    }

    case OP_DYN_INNER:
      push(vm, val_as_dyn(pop(vm))->inner);
      break;

    case OP_DYN_UPCAST: {
      uint16_t pair = READ_U16();
      ObjDyn *d = val_as_dyn(peek(vm, 0));
      VTable *to = NULL;
      // short by construction: one entry per supertrait the program upcasts
      // this table's trait to, which is almost always one.
      for (int i = 0; i < d->vtable->upcast_count && to == NULL; i++) {
        if (d->vtable->upcasts[i].pair == pair) {
          to = d->vtable->upcasts[i].to;
        }
      }
      if (to == NULL) {
        // codegen links every table of the source trait when it records the
        // pair, and every later one as it is built, so a miss means the image
        // disagrees with the code that reads it.
        VM_RETURN(runtime_error(vm, "trait object cannot be upcast"));
      }
      // the source stays on the stack across heap_dyn for the reason
      // OP_MAKE_DYN keeps its operand there: allocating may collect.
      ObjDyn *up = heap_dyn(vm->heap, d->inner, to);
      pop(vm);
      push(vm, val_obj((Obj *)up));
      break;
    }

    case OP_MATCH_FAIL:
      // the checker doesn't enforce match exhaustiveness yet, so this is a
      // real, reachable failure mode rather than a should-never-happen case.
      VM_RETURN(runtime_error(vm, "no match arm matched"));

    default:
      VM_RETURN(runtime_error(vm, "unknown opcode %d", (int)op));
    }
  }

#undef READ_BYTE
#undef READ_U16
#undef VM_RETURN
}

// ═══════════════════════════════════════════════════════════════════════════════
// Calling ducktape from C
// ═══════════════════════════════════════════════════════════════════════════════
//
// The other direction of the native boundary. A `@native` is C the language
// calls; these are what let that C call *back* — a comparator handed to a sort,
// and anything else whose shape is "the runtime does the work, the program
// decides one question inside it".
//
// The call is built exactly as `OP_CALL` builds one, because it is the same
// call: callee pushed, arguments above it, a frame over them, and the result
// left where the arguments were. What differs is only who drives the loop
// afterwards — `run(vm, depth)` returns to C at that frame's `OP_RETURN`
// instead of carrying on into a caller's bytecode.

// Keep `v` alive for the rest of this native. The VM's stack *is* the root set,
// so a root is a push: this is the only way a native can hold a heap value
// across a `native_call`, since anything living solely in a C local is
// invisible to the collector that the callee's own allocations may trigger.
void native_root(NativeCtx *ctx, Value v) {
  Vm *vm = ctx->vm;
  assert(vm != NULL && "native_root needs a VM; this native was called from C");
  push(vm, v);
}

// Drop the last `count` roots. Every `native_root` must be paired before the
// native returns — `OP_CALL` asserts the stack came back level.
void native_unroot(NativeCtx *ctx, int count) { ctx->vm->sp -= count; }

bool native_call(NativeCtx *ctx, Value callee, const Value *args, int argc,
                 Value *out) {
  Vm *vm = ctx->vm;
  assert(vm != NULL && "native_call needs a VM; this native was called from C");

  FunDef *fun;
  ObjClosure *closure = NULL;
  if (callee.kind == VAL_FUN) {
    // a plain function passed as a value — `xs.sort_by(compare_ints)` rather
    // than a closure literal. The two are one calling convention, which is why
    // the callee arrives here as a Value and not as an ObjClosure.
    fun = callee.as.fun;
  } else if (val_is_closure(callee)) {
    closure = val_as_closure(callee);
    fun = closure->fun;
  } else {
    ctx->unwinding = true;
    return runtime_error(vm, "can only call functions");
  }
  if (fun->param_count != argc) {
    ctx->unwinding = true;
    return runtime_error(vm,
                         "a call from the runtime passed %d argument%s to "
                         "'" SV_FMT "', which takes %d",
                         argc, argc == 1 ? "" : "s", SV_ARG(fun->name),
                         fun->param_count);
  }
  if (vm->sp - vm->stack >= STACK_MAX - STACK_HEADROOM) {
    ctx->unwinding = true;
    return runtime_error(vm, "value stack overflow");
  }

  push(vm, callee);
  for (int i = 0; i < argc; i++) {
    push(vm, args[i]);
  }

  if (fun->native != NULL) {
    NativeCtx inner = {.heap = vm->heap, .error = NULL, .vm = vm};
    Value result = fun->native(&inner, vm->sp - argc, argc);
    vm->sp -= argc + 1;
    if (inner.unwinding) {
      ctx->unwinding = true;
      return false;
    }
    if (inner.error != NULL) {
      ctx->unwinding = true;
      return runtime_error(vm, "%s", inner.error);
    }
    *out = result;
    return true;
  }

  if (fun->chunk == NULL) {
    ctx->unwinding = true;
    return runtime_error(vm, "call to uncompiled function '" SV_FMT "'",
                         SV_ARG(fun->name));
  }
  if (vm->frame_count >= FRAMES_MAX) {
    ctx->unwinding = true;
    return runtime_error(vm, "stack overflow");
  }

  int depth = vm->frame_count;
  vm->frames[vm->frame_count++] = (Frame){
      .fun = fun,
      .closure = closure,
      .ip = fun->chunk->code,
      .base = vm->sp - argc,
  };
  if (!run(vm, depth)) {
    // `run` reported it, with the frames that were live when it happened —
    // which is the whole reason the error is not re-raised here.
    ctx->unwinding = true;
    return false;
  }
  *out = pop(vm);
  return true;
}

bool vm_run(Executable *exe, Heap *heap, FunDef *entry) {
  assert(entry->chunk && "entry function was not compiled");
  if (entry->param_count != 0) {
    fprintf(stderr, "error: '" SV_FMT "' must take no parameters to be run\n",
            SV_ARG(entry->name));
    return false;
  }

  // one Vm per run, on the C stack: nothing outside this call has any business
  // reaching it, and a native that needs it is handed it through its context.
  Vm vm = {.exe = exe,
           .heap = heap,
           .sp = NULL,
           .frame_count = 0,
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

  bool ok = run(&vm, 0);
  heap->mark_roots = NULL; // no VM, so allocation stops triggering collection
  return ok;
}
