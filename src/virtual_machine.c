#include "virtual_machine.h"

#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "allocator.h"
#include "chunk.h"
#include "compiler.h"
#include "definition.h"
#include "garbage_collector.h"
#include "impl_registry.h"
#include "object.h"
#include "opcode.h"
#include "proto.h"
#include "string_utils.h"
#include "value.h"

#if defined(DEBUG_PRINT_CODE) || defined(DEBUG_TRACE_EXECUTION)
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

static void vm_reset_stack(VirtualMachine *vm) {
  vm->stack_top = vm->stack;
  vm->frame_count = 0;
}

// ===== error handling =====

void vm_panic(VirtualMachine *vm, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(vm->panic_message, sizeof(vm->panic_message), format, args);
  va_end(args);

  // print stack trace
  fprintf(stderr, "stack trace (most recent call last):\n");
  for (int i = vm->frame_count - 1; i >= 0; i--) {
    CallFrame *frame = &vm->frames[i];
    ObjFunction *fn = frame->function;
    size_t instruction = frame->ip - fn->chunk.code->data - 1;
    int line = fn->chunk.lines->data[instruction];

    fprintf(stderr, "  [line %d] in ", line);
    if (fn->name == NULL) {
      fprintf(stderr, "anonymous\n");
    } else {
      fprintf(stderr, "%.*s()\n", fn->name->str.length, fn->name->str.chars);
    }
  }

  // force enable gc
  vm->gc.disabled = 0;

  longjmp(vm->panic_jump, 1);
}

// ===== loading =====

typedef struct {
  ConstantLoader loader;
  VirtualMachine *vm;
} LoadContext;

static ObjFunction *vm_load_proto(ConstantLoader *loader, Proto *proto);

static Value load_constant(ConstantLoader *loader, Allocator *, const RawConstant *raw) {
  LoadContext *ctx = (LoadContext *)loader->ctx;
  VirtualMachine *vm = ctx->vm;

  switch (raw->type) {
  case RAW_NUMBER:
    return NUMBER_VAL(raw->as.number);
  case RAW_STRING: {
    ObjString *str = vm_intern_string(vm, sv_from_str(raw->as.string));
    assert(str != NULL && "interned string should not be NULL");
    return OBJECT_VAL(str);
  }
  case RAW_FUNCTION: {
    ObjFunction *function = vm_load_proto(loader, raw->as.function);
    assert(function != NULL && "loaded function should not be NULL");
    return OBJECT_VAL(function);
  }
  default:
    assert(false && "unknown constant type");
    return NIL_VAL; // unreachable
  }
}

static ObjFunction *vm_load_proto(ConstantLoader *loader, Proto *proto) {
  LoadContext *ctx = (LoadContext *)loader;
  VirtualMachine *vm = ctx->vm;

  ObjString *func_name =
      proto->name.length > 0 ? vm_intern_string(vm, sv_from_str(proto->name)) : NULL;
  ObjFunction *func = obj_function_new(&vm->al, func_name, loader, proto);
  vm_track_object(vm, &func->obj);

#ifdef DEBUG_PRINT_CODE
  char name_buf[256];
  obj_sprintf(name_buf, sizeof(name_buf), &func->obj);
  disassemble_chunk(&func->chunk, name_buf);
#endif

  return func;
}

// loader is for loading default impls of trait methods
static Object *vm_load_definition(VirtualMachine *vm, const Definition *def) {
  switch (def->type) {
  case DEFTYPE_STRUCT: {
    const StructDef *struct_def = &def->as.struct_def;
    ObjString *name = vm_intern_string(vm, sv_from_str(def->name));
    ObjStructDef *obj_def = obj_struct_def_new(name, &vm->al);
    vm_track_object(vm, &obj_def->obj);

    int field_count = arr_count(struct_def->field_names);
    for (int i = 0; i < field_count; i++) {
      ObjString *field_name = vm_intern_string(vm, sv_from_str(struct_def->field_names->data[i]));
      ht_put(&obj_def->fields, OBJECT_VAL(field_name), NUMBER_VAL(i), &vm->al);
    }

    return &obj_def->obj;
  }
  case DEFTYPE_TRAIT: {
    const TraitDef *trait_def = &def->as.trait_def;
    int method_count = arr_count(trait_def->methods);

    ObjString *name = vm_intern_string(vm, sv_from_str(def->name));
    ObjTraitDef *obj_def = obj_trait_def_new(name, method_count, &vm->al);
    vm_track_object(vm, &obj_def->obj);

    return &obj_def->obj;
  }
  default:
    assert(false && "unknown definition type");
    break;
  }
  return NULL; // unreachable
}

ObjFunction *vm_load(VirtualMachine *vm, Module *m) {
  gc_begin_staging(&vm->gc);

  int def_count = arr_count(m->def_reg.defs);
  for (int i = 0; i < def_count; i++) {
    Definition *def = &m->def_reg.defs->data[i];
    if (def->slot < arr_count(vm->definition_objects)) {
      continue; // already loaded
    }

    while (arr_count(vm->definition_objects) <= def->slot) {
      arr_push(vm->definition_objects, NULL, &vm->al);
      arr_push(vm->definitions, (Definition){0}, &vm->al);
    }

    Object *def_obj = vm_load_definition(vm, def);
    def_obj->flags.def_id = DEFID_START + def->slot;
    vm->definition_objects->data[def->slot] = def_obj;
    vm->definitions->data[def->slot] = def_copy(def, &vm->al);
  }

  LoadContext ctx = {
      .loader = {.load_constant = load_constant, .ctx = &ctx},
      .vm = vm,
  };
  ObjFunction *main = vm_load_proto(&ctx.loader, m->proto);

  vm_push(vm, OBJECT_VAL(main));
  gc_end_staging(&vm->gc);
  vm_pop(vm);

  return main;
}

// ===== operations =====

static void vm_concat(VirtualMachine *vm) {
  gc_begin_staging(&vm->gc);
  ObjString *b = AS_STRING(vm_pop(vm));
  ObjString *a = AS_STRING(vm_pop(vm));

  String r = str_concat(&a->str, &b->str, &vm->al);
  uint32_t hash = sv_hash(sv_from_str(r));
  ObjString *str = obj_string_new(r, hash, &vm->al);
  vm_track_object(vm, &str->obj);

  vm_push(vm, OBJECT_VAL(str));
  gc_end_staging(&vm->gc);
}

static ObjUpvalue *vm_capture_upvalue(VirtualMachine *vm, Value *local) {
  ObjUpvalue *prev_upvalue = NULL;
  ObjUpvalue *upvalue = vm->open_upvalues;

  while (upvalue != NULL && upvalue->location > local) {
    prev_upvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue *new_upvalue = obj_upvalue_new(&vm->al, local);
  vm_track_object(vm, &new_upvalue->obj);
  new_upvalue->next = upvalue;

  if (prev_upvalue == NULL) {
    vm->open_upvalues = new_upvalue;
  } else {
    prev_upvalue->next = new_upvalue;
  }

  return new_upvalue;
}

static void vm_close_upvalues(VirtualMachine *vm, Value *last) {
  // close all open upvalues that capture a local variable at or above `last`.
  while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
    ObjUpvalue *upvalue = vm->open_upvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm->open_upvalues = upvalue->next;
  }
}

static Value *vm_property_ref(VirtualMachine *vm, uint8_t pic_index, Value receiver,
                              ObjFunction *function) {
  (void)vm;
  ObjStructInst *inst;
  if (IS_STRUCT_INST(receiver)) {
    inst = AS_STRUCT_INST(receiver);
  } else if (IS_TRAIT_OBJECT(receiver)) {
    Object *inner = AS_TRAIT_OBJECT(receiver)->receiver;
    assert(inner->type == OBJ_STRUCT_INST && "trait object receiver must be a struct instance");
    inst = (ObjStructInst *)inner;
  } else {
    vm_panic(vm, "invalid receiver type for property access");
  }

  Pic *pic = &function->pic[pic_index];
  uint16_t shape_id = inst->def->obj.flags.def_id;

  if (pic->count < PIC_CACHE_SIZE) {
    for (int i = 0; i < pic->count; i++) {
      if (pic->entries[i].shape_id == shape_id)
        return &inst->payload[pic->entries[i].offset];
    }

    ObjString *field_name = AS_STRING(function->chunk.constants->data[pic->name]);
    Value *offset_val = ht_get(&inst->def->fields, OBJECT_VAL(field_name));
    if (!offset_val) {
      return NULL;
    }

    uint8_t offset = (uint8_t)AS_NUMBER(*offset_val);
    pic->entries[pic->count++] = (PicEntry){.shape_id = shape_id, .offset = offset};
    return &inst->payload[offset];
  }

  ObjString *field_name = AS_STRING(function->chunk.constants->data[pic->name]);
  Value *offset_val = ht_get(&inst->def->fields, OBJECT_VAL(field_name));
  if (!offset_val) {
    return NULL;
  }

  return &inst->payload[(uint8_t)AS_NUMBER(*offset_val)];
}

static Object *vm_method_ref(VirtualMachine *vm, uint8_t pic_index, Value receiver,
                             ObjFunction *function) {
  (void)vm;
  Pic *pic = &function->pic[pic_index];
  ObjImpl *impl = NULL;

  if (!IS_OBJECT(receiver)) {
    vm_panic(vm, "invalid receiver type for method access");
  }

  switch (AS_OBJECT(receiver)->type) {
  case OBJ_TRAIT_OBJECT: {
    ObjTraitObject *trait_object = AS_TRAIT_OBJECT(receiver);
    impl = trait_object->impl;
    break;
  }
  case OBJ_STRUCT_DEF: {
    impl = AS_STRUCT_DEF(receiver)->inherent_impl;
    break;
  }
  case OBJ_STRUCT_INST: {
    ObjStructInst *inst = AS_STRUCT_INST(receiver);
    impl = inst->def->inherent_impl;
    break;
  }
  default:
    vm_panic(vm, "invalid receiver type for method access");
    break;
  }

  ObjString *method_name = AS_STRING(function->chunk.constants->data[pic->name]);

  // resolve the canonical impl up front by scanning for the method name.
  // this also gives us the slot index and the single shape_id to key on.
  for (int i = 0; i < impl->trait->method_count; i++) {
    // this might be very slow
    ObjImpl *target_impl = (impl->impls[i] != NULL) ? impl : impl->trait->default_impl;

    assert(target_impl != NULL && target_impl->impls[i] != NULL &&
           "impl slot must be filled after default resolution");

    Object *slot = target_impl->impls[i];
    assert(slot->type == OBJ_FUNCTION && "invalid method type in impl");

    ObjFunction *method = (ObjFunction *)slot;
    if (!obj_equal(&method->name->obj, &method_name->obj)) {
      continue;
    }

    uint16_t shape_id = (uint16_t)target_impl->subject->flags.def_id;

    if (pic->count < PIC_CACHE_SIZE) {
      for (int j = 0; j < pic->count; j++) {
        if (pic->entries[j].shape_id == shape_id)
          return target_impl->impls[pic->entries[j].offset];
      }
      pic->entries[pic->count++] = (PicEntry){.shape_id = shape_id, .offset = (uint8_t)i};
    } else {
      pic->count = UINT8_MAX; // megamorphic
    }

    return &method->obj;
  }

  return NULL;
}

Object *vm_cast_struct(VirtualMachine *vm, Value receiver_val, ObjStructDef *def) {
  Object *receiver = AS_OBJECT(receiver_val);
  if (receiver->type == OBJ_TRAIT_OBJECT) {
    ObjTraitObject *trait_object = (ObjTraitObject *)receiver;
    receiver = trait_object->receiver; // unwrap
  }
  if (receiver->type == OBJ_STRUCT_INST) {
    ObjStructInst *inst = (ObjStructInst *)receiver;
    if (inst->def == def) {
      return receiver; // already the correct struct instance
    }
  }

  vm_panic(vm, "expected instance of struct '%.*s' but got %s", def->name->str.length,
           def->name->str.chars, val_type_name(receiver_val.type));
  return NULL; // unreachable
}

Object *vm_cast_trait(VirtualMachine *vm, Value receiver_val, ObjTraitDef *trait) {
  Object *receiver = AS_OBJECT(receiver_val);
  if (receiver->type == OBJ_TRAIT_OBJECT) {
    ObjTraitObject *trait_object = (ObjTraitObject *)receiver;
    if (trait_object->impl->trait == trait) {
      return receiver; // already a trait object of the correct trait
    }
    // unwrap
    receiver = trait_object->receiver;
  }

  ObjImpl *impl = impl_registry_get(&vm->impl_registry, receiver, trait);

  if (impl == NULL) {
    char inst_print[32];
    val_sprintf(inst_print, sizeof(inst_print), receiver_val);
    vm_panic(vm, "'%s' does not implement trait '%.*s'", inst_print, trait->name->str.length,
             trait->name->str.chars);
  }

  ObjTraitObject *trait_object = obj_trait_object_new(receiver, impl, &vm->al);
  vm_track_object(vm, &trait_object->obj);
  return &trait_object->obj;
}

// panic if cast fails
// returns null if target type is not supported (caller should handle this)
Object *vm_cast(VirtualMachine *vm, Value receiver_val, Object *target) {
  if (target->type == OBJ_STRUCT_DEF) {
    return vm_cast_struct(vm, receiver_val, (ObjStructDef *)target);
  } else if (target->type == OBJ_TRAIT_DEF) {
    return vm_cast_trait(vm, receiver_val, (ObjTraitDef *)target);
  } else {
    return NULL; // caller handle error
  }
}

// ===== vm execution =====

static void vm_call_function(VirtualMachine *vm, ObjFunction *callee, int arg_count) {
  if (vm->frame_count == VM_FRAMES_MAX) {
    vm_panic(vm, "stack overflow");
  }

  if (arg_count != callee->arity) {
    vm_panic(vm, "expected %d arguments but got %d", callee->arity, arg_count);
  }

  int constraint_count = arr_count(callee->param_constraints);
  for (int i = 0; i < constraint_count; i++) {
    int def_idx = callee->param_constraints->data[i];
    if (def_idx == -1) {
      continue; // no constraint for this parameter
    }

    Value arg = vm_peek(vm, arg_count - 1 - i);
    Object *def_obj = vm->definition_objects->data[def_idx];

    Object *casted = vm_cast(vm, arg, def_obj);
    if (casted) {
      vm->stack_top[i - arg_count] = OBJECT_VAL(casted);
    } else {
      vm_panic(vm, "invalid constraint on parameter %d", i);
    }
  }

  CallFrame *frame = &vm->frames[vm->frame_count++];
  frame->function = callee;
  frame->ip = callee->chunk.code->data;
  frame->base = vm->stack_top - callee->arity - 1;
}

static void vm_call_native(VirtualMachine *vm, ObjNative *native, int arg_count) {
  Value result = native->func(vm, arg_count, vm->stack_top - arg_count);
  vm->stack_top -= arg_count + 1;
  vm_push(vm, result);
}

static void vm_call_closure(VirtualMachine *vm, ObjClosure *closure, int arg_count) {
  vm_call_function(vm, closure->func, arg_count);
  vm->frames[vm->frame_count - 1].upvalues = closure->upvalues;
}

static void vm_call_struct_constructor(VirtualMachine *vm, ObjStructDef *def, int arg_count) {
  if (arg_count != def->fields.count) {
    vm_panic(vm, "expected %d arguments but got %d", def->fields.count, arg_count);
  }

  gc_begin_staging(&vm->gc);
  ObjStructInst *inst = obj_struct_inst_new(def, &vm->al);
  vm_track_object(vm, &inst->obj);
  for (int i = def->fields.count - 1; i >= 0; i--) {
    inst->payload[i] = vm_pop(vm);
  }

  vm_pop(vm); // pop the struct def
  vm_push(vm, OBJECT_VAL(inst));

  gc_end_staging(&vm->gc);
}

static void vm_call_object(VirtualMachine *vm, Object *callee, int arg_count) {
  switch (callee->type) {
  case OBJ_FUNCTION:
    vm_call_function(vm, (ObjFunction *)callee, arg_count);
    return;
  case OBJ_NATIVE:
    vm_call_native(vm, (ObjNative *)callee, arg_count);
    return;
  case OBJ_CLOSURE:
    vm_call_closure(vm, (ObjClosure *)callee, arg_count);
    return;
  case OBJ_STRUCT_DEF:
    vm_call_struct_constructor(vm, (ObjStructDef *)callee, arg_count);
    return;
  default:
    break; // not callable
  }

  vm_panic(vm, "%s is not callable", obj_type_name(callee->type));
}

static void vm_call(VirtualMachine *vm, Value callee, int arg_count) {
  if (IS_OBJECT(callee)) {
    vm_call_object(vm, AS_OBJECT(callee), arg_count);
    return;
  }

  vm_panic(vm, "%s is not callable", val_type_name(callee.type));
}

static void vm_run(VirtualMachine *vm) {
  CallFrame *frame = &vm->frames[vm->frame_count - 1];
  uint8_t *ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)(ip[-2] << 8 | ip[-1]))
#define READ_CONSTANT(idx) (frame->function->chunk.constants->data[(idx)])
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
      printf(" %c", frame->base == slot ? '|' : ']');
    }
    printf("\n");
    disassemble_chunk_instruction(&frame->function->chunk,
                                  (int)(ip - frame->function->chunk.code->data));
#endif

    uint8_t instruction = READ_BYTE();
    switch (instruction) {
    case OP_CONSTANT:
      Value constant = READ_CONSTANT(READ_BYTE());
      vm_push(vm, constant);
      break;
    case OP_CLOSURE: {
      Value func_constant = READ_CONSTANT(READ_BYTE());
      assert(IS_FUNCTION(func_constant) && "expect function constant");
      ObjFunction *function = AS_FUNCTION(func_constant);

      gc_begin_staging(&vm->gc);
      ObjClosure *closure = obj_closure_new(function, &vm->al);
      vm_track_object(vm, &closure->obj);

      int upvalue_count = function->upvalue_count;
      for (int i = 0; i < upvalue_count; i++) {
        uint8_t is_local = READ_BYTE();
        uint8_t idx = READ_BYTE();
        if (is_local) {
          closure->upvalues[i] = vm_capture_upvalue(vm, frame->base + idx);
        } else {
          closure->upvalues[i] = frame->upvalues[idx];
        }
      }
      vm_push(vm, OBJECT_VAL(closure));
      gc_end_staging(&vm->gc);
      break;
    }
    case OP_BYTE:
      vm_push(vm, NUMBER_VAL(READ_BYTE()));
      break;
    case OP_POP:
      vm_pop(vm);
      break;
    case OP_DUP: {
      vm_push(vm, vm_peek(vm, 0));
      break;
    }
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
      vm->stack_top[-1] = BOOL_VAL(val_is_falsey(vm_peek(vm, 0)));
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
      Value name_val = READ_CONSTANT(name_idx);
      if (ht_get(&vm->globals, name_val)) {
        vm_panic(vm, "variable '%s' is already defined", AS_STRING(name_val)->str.chars);
      }
      ht_put(&vm->globals, name_val, vm_peek(vm, 0), &vm->al);
      vm_pop(vm);
      break;
    case OP_GET_GLOBAL: {
      uint8_t name_idx = READ_BYTE();
      Value name_val = READ_CONSTANT(name_idx);
      Value *value = ht_get(&vm->globals, name_val);
      if (!value) {
        vm_panic(vm, "undefined variable '%s'", AS_STRING(name_val)->str.chars);
      }
      vm_push(vm, *value);
      break;
    }
    case OP_SET_GLOBAL: {
      uint8_t name_idx = READ_BYTE();
      Value name_val = READ_CONSTANT(name_idx);
      Value *value = ht_get(&vm->globals, name_val);
      if (!value) {
        vm_panic(vm, "undefined variable '%s'", AS_STRING(name_val)->str.chars);
      }
      *value = vm_peek(vm, 0);
      break;
    }
    case OP_GET_LOCAL: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, frame->base[slot]);
      break;
    }
    case OP_SET_LOCAL: {
      uint8_t slot = READ_BYTE();
      frame->base[slot] = vm_peek(vm, 0);
      break;
    }
    case OP_GET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      vm_push(vm, *frame->upvalues[slot]->location);
      break;
    }
    case OP_SET_UPVALUE: {
      uint8_t slot = READ_BYTE();
      *frame->upvalues[slot]->location = vm_peek(vm, 0);
      break;
    }
    case OP_CLOSE_UPVALUE: {
      vm_close_upvalues(vm, vm->stack_top - 1);
      vm_pop(vm);
      break;
    }
    case OP_GET_DEFINITION: {
      uint8_t def_idx = READ_BYTE();
      Object *def_obj = vm->definition_objects->data[def_idx];
      if (!def_obj) {
        vm_panic(vm, "undefined definition at index %d", def_idx);
      }
      vm_push(vm, OBJECT_VAL(def_obj));
      break;
    }
    case OP_GET_PROPERTY: {
      uint8_t pic_index = READ_BYTE();
      Value receiver = vm_peek(vm, 0);
      Value *property = vm_property_ref(vm, pic_index, receiver, frame->function);
      if (!property) {
        ObjString *field_name =
            AS_STRING(frame->function->chunk.constants->data[frame->function->pic[pic_index].name]);
        vm_panic(vm, "undefined property '%.*s'", field_name->str.length, field_name->str.chars);
      }
      vm->stack_top[-1] = *property;
      break;
    }
    case OP_SET_PROPERTY: {
      uint8_t pic_index = READ_BYTE();
      Value receiver = vm_peek(vm, 1);
      Value *property = vm_property_ref(vm, pic_index, receiver, frame->function);
      if (!property) {
        ObjString *field_name =
            AS_STRING(frame->function->chunk.constants->data[frame->function->pic[pic_index].name]);
        vm_panic(vm, "undefined property '%.*s'", field_name->str.length, field_name->str.chars);
      }
      *property = vm_peek(vm, 0);
      vm->stack_top[-2] = *property;
      vm->stack_top--;
      break;
    }
    case OP_LOOP: {
      uint16_t offset = READ_SHORT();
      ip -= offset;
      break;
    }
    case OP_JUMP: {
      uint16_t offset = READ_SHORT();
      ip += offset;
      break;
    }
    case OP_JUMP_IF_FALSE: {
      uint16_t offset = READ_SHORT();
      if (val_is_falsey(vm_peek(vm, 0))) {
        ip += offset;
      }
      break;
    }
    case OP_CALL: {
      uint8_t arg_count = READ_BYTE();
      Value callee = vm_peek(vm, arg_count);
      frame->ip = ip;
      vm_call(vm, callee, arg_count);
      frame = &vm->frames[vm->frame_count - 1];
      ip = frame->ip;
      break;
    }
    case OP_INVOKE: {
      uint8_t pic_index = READ_BYTE();
      uint8_t arg_count = READ_BYTE();
      Value receiver = vm_peek(vm, arg_count);
      Object *method = vm_method_ref(vm, pic_index, receiver, frame->function);
      if (method == NULL) {
        ObjString *method_name =
            AS_STRING(frame->function->chunk.constants->data[frame->function->pic[pic_index].name]);
        vm_panic(vm, "undefined method '%.*s' ", method_name->str.length, method_name->str.chars);
      }
      frame->ip = ip;
      vm_call_object(vm, method, arg_count);
      frame = &vm->frames[vm->frame_count - 1];
      ip = frame->ip;
      break;
    }
    case OP_IMPL: {
      Value trait = vm_peek(vm, 1);
      Value subject = vm_peek(vm, 0);

      assert(IS_TRAIT_DEF(trait) && "expected trait definition");
      assert((IS_STRUCT_DEF(subject) || IS_TRAIT_DEF(subject)) &&
             "expect struct or trait definition as impl subject");

      ObjTraitDef *trait_def = AS_TRAIT_DEF(trait);

      gc_begin_staging(&vm->gc);
      ObjImpl *impl = obj_impl_new(AS_TRAIT_DEF(trait), AS_OBJECT(subject), &vm->al);
      vm_track_object(vm, &impl->obj);

      if (!impl_registry_put(&vm->impl_registry, impl, &vm->al)) {
        vm_panic(
            vm, "duplicate impl for trait '%.*s' and subject '%.*s'", trait_def->name->str.length,
            trait_def->name->str.chars,
            AS_OBJECT(subject)->type == OBJ_STRUCT_DEF ? AS_STRUCT_DEF(subject)->name->str.length
                                                       : AS_TRAIT_DEF(subject)->name->str.length,
            AS_OBJECT(subject)->type == OBJ_STRUCT_DEF ? AS_STRUCT_DEF(subject)->name->str.chars
                                                       : AS_TRAIT_DEF(subject)->name->str.chars);
      }

      if (IS_TRAIT_DEF(subject)) {
        AS_TRAIT_DEF(subject)->default_impl = impl;
      }

      if (IS_STRUCT_DEF(subject)) {
        AS_STRUCT_DEF(subject)->inherent_impl = impl;
      }

      vm_pop(vm); // pop subject
      vm_pop(vm); // pop trait
      vm_push(vm, OBJECT_VAL(impl));

      gc_end_staging(&vm->gc);
      break;
    }
    case OP_IMPL_METHOD: {
      uint8_t method_idx = READ_BYTE();

      Value impl_val = vm_peek(vm, 1);
      Value method_val = vm_peek(vm, 0);

      assert(IS_IMPL(impl_val) && "expected impl");
      assert((IS_FUNCTION(method_val)) && "expected function as impl method");

      ObjImpl *impl = AS_IMPL(impl_val);

      if (method_idx >= impl->trait->method_count) {
        // this should not happen if the compiler is correct
        vm_panic(vm, "internal error: cannot find method to implement");
      }

      vm_pop(vm); // pop method
      impl->impls[method_idx] = AS_OBJECT(method_val);

      break;
    }
    case OP_CAST: {
      Value receiver_val = vm_peek(vm, 1);
      Object *target = AS_OBJECT(vm_peek(vm, 0));
      Object *casted = vm_cast(vm, receiver_val, target);
      if (!casted) {
        vm_panic(vm, "cannot cast to type '%s'", obj_type_name(target->type));
      }
      vm_pop(vm);                             // pop target
      vm->stack_top[-1] = OBJECT_VAL(casted); // replace receiver with casted value
      break;
    }
    case OP_RETURN:
      Value result = vm_pop(vm);
      vm_close_upvalues(vm, frame->base);
      frame->ip = ip;
      vm->frame_count--;
      if (vm->frame_count == 0) {
        vm_pop(vm); // pop main script function
        return;
      }
      vm->stack_top = frame->base; // discard local variables
      vm_push(vm, result);         // push return value
      frame = &vm->frames[vm->frame_count - 1];
      ip = frame->ip;
      break;
    case OP_TRAP:
      raise(SIGTRAP);
      break;
    default:
      assert(false && "unknown opcode");
      vm_panic(vm, "unknown opcode %d", instruction);
    }
  }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef BINARY_OP
}

static void vm_execute(VirtualMachine *vm, ObjFunction *function) {
  vm_push(vm, OBJECT_VAL(function));
  vm_call_function(vm, function, 0);

  assert(vm->gc.disabled == 0 && "gc should not be disabled when starting execution");
  vm_run(vm);
}

// ===== initialization & cleanup =====

void vm_init(VirtualMachine *vm, Allocator al) {
  vm_reset_stack(vm);

  vm->inner_al = al;

  gc_init(&vm->gc, vm, &vm->inner_al);
  vm->al = gc_allocator_create(&vm->gc);

  vm->objects = NULL;

  ht_init(&vm->strings);
  ht_init(&vm->globals);

  vm->open_upvalues = NULL;

  vm->slot_al.next_slot = 0;
  vm->definition_objects = arr_new();
  vm->definitions = arr_new();

  impl_registry_init(&vm->impl_registry);
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

  int def_count = arr_count(vm->definitions);
  for (int i = 0; i < def_count; i++) {
    def_destroy(&vm->definitions->data[i], &vm->al);
  }

  arr_free(&vm->definitions, &vm->al);
  arr_free(&vm->definition_objects, &vm->al);

  impl_registry_destroy(&vm->impl_registry, &vm->al);

  assert(vm->gc.bytes_allocated == 0 && "memory leak detected: garbage collector "
                                        "has allocated memory that was not freed");

  gc_destroy(&vm->gc);
}

// ===== public =====

ObjString *vm_intern_string(VirtualMachine *vm, StringView str) {
  uint32_t hash = sv_hash(str);
  ObjString tmp = {
      .obj = {.type = OBJ_STRING, .flags = {0}, .next = NULL},
      .str = {.chars = (char *)str.chars, .length = str.length, .capacity = str.length},
      .hash = hash,
  };
  Value *existing = ht_get(&vm->strings, OBJECT_VAL(&tmp));
  if (existing) {
    return AS_STRING(*existing);
  }
  gc_begin_staging(&vm->gc);
  String copy = str_copy_sv(str, &vm->al);
  ObjString *interned = obj_string_new(copy, hash, &vm->al);
  vm_track_object(vm, &interned->obj);
  interned->obj.flags.interned = true;
  ht_put(&vm->strings, OBJECT_VAL(interned), OBJECT_VAL(interned), &vm->al);
  gc_end_staging(&vm->gc);
  return interned;
}

void vm_track_object(VirtualMachine *vm, Object *object) {
  assert(object != NULL && "cannot track NULL object");
  assert((object->next == NULL && vm->objects != object) && "object is already tracked");
  object->next = vm->objects;
  vm->objects = object;
}

void vm_define_native(VirtualMachine *vm, StringView name, NativeFunc function) {
  gc_begin_staging(&vm->gc);
  ObjString *str = vm_intern_string(vm, name);
  ObjNative *native = obj_native_new(function, &vm->al);
  vm_track_object(vm, &native->obj);
  ht_put(&vm->globals, OBJECT_VAL(str), OBJECT_VAL(native), &vm->al);
  gc_end_staging(&vm->gc);
}

InterpretResult vm_interpret(VirtualMachine *vm, const char *source) {
  Module m = {0};
  if (!compile(source, vm->definitions, &vm->slot_al, &vm->inner_al, &m)) {
    return INTERPRET_COMPILE_ERROR;
  }

  if (setjmp(vm->panic_jump) != 0) {
    vm_reset_stack(vm);
    fprintf(stderr, "panic: %s\n", vm->panic_message);
    module_destroy(&m, &vm->inner_al);
    return INTERPRET_RUNTIME_ERROR;
  }

  ObjFunction *main = vm_load(vm, &m);
  module_destroy(&m, &vm->inner_al);

  vm_execute(vm, main);

  return INTERPRET_OK;
}