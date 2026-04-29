#include "virtual_machine.h"

#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "allocator.h"
#include "chunk.h"
#include "compiler.h"
#include "object.h"
#include "opcode.h"
#include "string_utils.h"
#include "value.h"

#if defined(DEBUG_PRINT_CODE)
#include "chunk_debug.h"
#endif

// ===== stack operations =====

static void vm_push(VirtualMachine *vm, Value value) {
  assert(vm->stack_top < vm->stack + VM_STACK_MAX && "stack overflow");
  *vm->stack_top++ = value;
}

static inline Value vm_pop(VirtualMachine *vm) {
  assert(vm->stack_top > vm->stack && "stack underflow");
  return *--vm->stack_top;
}

static inline Value vm_peek(VirtualMachine *vm, int distance) {
  assert(vm->stack_top - distance - 1 >= vm->stack && "stack underflow");
  return vm->stack_top[-1 - distance];
}

static void vm_reset_stack(VirtualMachine *vm) { vm->stack_top = vm->stack; }

// ===== error handling =====

void vm_panic(VirtualMachine *vm, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(vm->panic_message, sizeof(vm->panic_message), format, args);
  va_end(args);

  longjmp(vm->panic_jump, 1);
}

// ===== constant loading =====

static Value load_constant(ConstantLoader *loader, Allocator *, const RawConstant *raw) {
  VirtualMachine *vm = (VirtualMachine *)loader->ctx;
  switch (raw->type) {
  case RAW_NUMBER:
    return NUMBER_VAL(raw->as.number);
  case RAW_STRING: {
    ObjString *str = vm_intern_string(vm, sv_from_str(raw->as.string));
    assert(str != NULL && "interned string should not be NULL");
    return OBJECT_VAL(str);
  }
  default:
    assert(false && "unknown constant type");
    return NIL_VAL; // unreachable
  }
}

static void vm_load_proto(VirtualMachine *vm, Proto *proto) {
  ConstantLoader loader = {
      .load_constant = load_constant,
      .ctx = vm,
  };

  chunk_load_from_proto(&vm->chunk, proto, &loader, &vm->al);

#ifdef DEBUG_PRINT_CODE
  disassemble_chunk(&vm->chunk, "<main>");
#endif
}

// ===== operations =====

static void vm_concat(VirtualMachine *vm) {
  ObjString *b = AS_STRING(vm_peek(vm, 0));
  ObjString *a = AS_STRING(vm_peek(vm, 1));

  String r = str_concat(&a->str, &b->str, &vm->al);
  uint32_t hash = sv_hash(sv_from_str(r));
  ObjString *str = obj_string_new(r, hash, &vm->al);

  vm_track_object(vm, &str->obj);

  vm_pop(vm);
  vm_pop(vm);
  vm_push(vm, OBJECT_VAL(str));
}

// ===== vm execution =====

static void vm_run(VirtualMachine *vm) {
  uint8_t *ip = vm->chunk.code->data;

#define READ_BYTE() (*ip++)
#define READ_CONSTANT(idx) (vm->chunk.constants->data[(idx)])
#define BINARY_OP(val, op)                                                                         \
  if (!IS_NUMBER(vm_peek(vm, 0)) || !IS_NUMBER(vm_peek(vm, 1))) {                                  \
    vm_panic(vm, "operands must be numbers");                                                      \
  }                                                                                                \
  do {                                                                                             \
    double b = AS_NUMBER(vm_pop(vm));                                                              \
    double a = AS_NUMBER(vm_pop(vm));                                                              \
    vm_push(vm, val(a op b));                                                                      \
  } while (false)

  while (true) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value *slot = vm->stack; slot < vm->stack_top; slot++) {
      printf("[ ");
      val_print(*slot);
      printf(" ]");
    }
    printf("\n");
    disassemble_chunk_instruction(&vm->chunk, (int)(ip - vm->chunk.code->data));
#endif

    uint8_t instruction = READ_BYTE();
    switch (instruction) {
    case OP_CONSTANT:
      Value constant = READ_CONSTANT(READ_BYTE());
      vm_push(vm, constant);
      break;
    case OP_POP:
      vm_pop(vm);
      break;
    case OP_ADD: {
      if (IS_NUMBER(vm_peek(vm, 0)) && IS_NUMBER(vm_peek(vm, 1))) {
        double b = AS_NUMBER(vm_pop(vm));
        double a = AS_NUMBER(vm_pop(vm));
        vm_push(vm, NUMBER_VAL(a + b));
        break;
      }
      if (IS_STRING(vm_peek(vm, 0)) && IS_STRING(vm_peek(vm, 1))) {
        vm_concat(vm);
        break;
      }
      vm_panic(vm, "operands must be two numbers or two strings");
      break;
    }
    case OP_SUBTRACT:
      BINARY_OP(NUMBER_VAL, -);
      break;
    case OP_MULTIPLY:
      BINARY_OP(NUMBER_VAL, *);
      break;
    case OP_DIVIDE:
      BINARY_OP(NUMBER_VAL, /);
      break;
    case OP_NEGATE:
      if (!IS_NUMBER(vm->stack_top[-1])) {
        vm_panic(vm, "operand must be a number");
      }
      vm->stack_top[-1] = NUMBER_VAL(-AS_NUMBER(vm_peek(vm, 0)));
      break;
    case OP_NOT:
      vm->stack_top[-1] = BOOL_VAL(val_falsey(vm_peek(vm, 0)));
      break;
    case OP_EQUAL:
      Value b = vm_pop(vm);
      Value a = vm_pop(vm);
      vm_push(vm, BOOL_VAL(val_equal(a, b)));
      break;
    case OP_GREATER:
      BINARY_OP(BOOL_VAL, >);
      break;
    case OP_LESS:
      BINARY_OP(BOOL_VAL, <);
      break;
    case OP_NIL:
      vm_push(vm, NIL_VAL);
      break;
    case OP_TRUE:
      vm_push(vm, BOOL_VAL(true));
      break;
    case OP_FALSE:
      vm_push(vm, BOOL_VAL(false));
      break;
    case OP_DEFINE_GLOBAL:
      uint8_t name_idx = READ_BYTE();
      Value name_val = vm->chunk.constants->data[name_idx];
      if (ht_get(&vm->globals, name_val)) {
        vm_panic(vm, "variable '%s' is already defined", AS_STRING(name_val)->str.chars);
      }
      ht_put(&vm->globals, name_val, vm_peek(vm, 0), &vm->al);
      vm_pop(vm);
      break;
    case OP_GET_GLOBAL: {
      uint8_t name_idx = READ_BYTE();
      Value name_val = vm->chunk.constants->data[name_idx];
      Value *value = ht_get(&vm->globals, name_val);
      if (!value) {
        vm_panic(vm, "undefined variable '%s'", AS_STRING(name_val)->str.chars);
      }
      vm_push(vm, *value);
      break;
    }
    case OP_SET_GLOBAL: {
      uint8_t name_idx = READ_BYTE();
      Value name_val = vm->chunk.constants->data[name_idx];
      Value *value = ht_get(&vm->globals, name_val);
      if (!value) {
        vm_panic(vm, "undefined variable '%s'", AS_STRING(name_val)->str.chars);
      }
      *value = vm_peek(vm, 0);
      break;
    }
    case OP_GET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, vm->stack[slot]);
      break;
    }
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm->stack[slot] = vm_peek(vm, 0);
      break;
    }
    case OP_RETURN:
      return;
    case OP_TRAP:
      raise(SIGTRAP);
      break;
    default:
      assert(false && "unknown opcode");
    }
  }
#undef READ_BYTE
#undef READ_CONSTANT
#undef BINARY_OP
}

// ===== initialization & cleanup =====

void vm_init(VirtualMachine *vm, Allocator al) {
  vm_reset_stack(vm);
  vm->al = al;
  vm->objects = NULL;

  ht_init(&vm->strings);
  ht_init(&vm->globals);
}

void vm_destroy(VirtualMachine *vm) {
  vm_reset_stack(vm);

  Object *obj = vm->objects;
  while (obj) {
    Object *next = obj->next;
    obj_free(&obj, &vm->al);
    obj = next;
  }

  ht_destroy(&vm->strings, &vm->al);
  ht_destroy(&vm->globals, &vm->al);

  chunk_destroy(&vm->chunk, &vm->al);
}

// ===== object management =====

ObjString *vm_intern_string(VirtualMachine *vm, StringView str) {
  uint32_t hash = sv_hash(str);
  ObjString tmp = {
      .obj = {.type = OBJ_STRING, .flags = 0, .next = NULL},
      .str = {.chars = (char *)str.chars, .length = str.length, .capacity = str.length},
      .hash = hash,
  };
  Value *existing = ht_get(&vm->strings, OBJECT_VAL(&tmp));
  if (existing) {
    return AS_STRING(*existing);
  }
  String copy = str_copy_sv(str, &vm->al);
  ObjString *interned = obj_string_new(copy, hash, &vm->al);
  obj_string_set_interned(interned, true);
  vm_track_object(vm, &interned->obj);
  ht_put(&vm->strings, OBJECT_VAL(interned), OBJECT_VAL(interned), &vm->al);
  return interned;
}

void vm_track_object(VirtualMachine *vm, Object *object) {
  assert(object != NULL && "cannot track NULL object");
  assert(object->next == NULL && "object is already tracked");
  object->next = vm->objects;
  vm->objects = object;
}

// ===== public api =====

InterpretResult vm_interpret(VirtualMachine *vm, const char *source) {
  Proto *proto = compile(source, &vm->al);
  if (!proto) {
    return INTERPRET_COMPILE_ERROR;
  }

  if (setjmp(vm->panic_jump) != 0) {
    vm_reset_stack(vm);
    fprintf(stderr, "panic: %s\n", vm->panic_message);
    return INTERPRET_RUNTIME_ERROR;
  }

  vm_load_proto(vm, proto);
  proto_destroy(proto, &vm->al);
  al_free_for(&vm->al, proto);

  vm_run(vm);

  return INTERPRET_OK;
}