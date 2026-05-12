#pragma once

#include "allocator.h"
#include "builtins.h"
#include "compiler.h"
#include "definition.h"
#include "garbage_collector.h"
#include "hash_table.h"
#include "impl_registry.h"
#include "object.h"
#include "value.h"

#include <setjmp.h>

#define VM_FRAMES_MAX 64

#define VM_STACK_MAX 2048

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

typedef struct {
  ObjFunction *function;
  ObjUpvalue **upvalues;

  uint8_t *ip;
  Value *base;
} CallFrame;

typedef struct VirtualMachine {
  Value *stack_top;
  Value stack[VM_STACK_MAX];
  int frame_count;
  CallFrame frames[VM_FRAMES_MAX];

  BuiltinRegistry builtins;

  Object *objects; // linked list
  HashTable strings;
  HashTable globals;

  SlotAllocator slot_al;              // for assigning definition slots
  DefinitionArray *definitions;       // all definitions in the program
  ObjectPtrArray *definition_objects; // parallel array to definitions
  ImplRegistry impl_registry;

  ObjUpvalue *open_upvalues; // linked list

  jmp_buf panic_jump;
  char panic_message[256];

  GarbageCollector gc;
  Allocator al; // gc allocator

  Allocator inner_al;
} VirtualMachine;

typedef struct {
  const char *name;
  NativeFunc func;
} NativeMethodDef;

void vm_init(VirtualMachine *vm, Allocator al);

void vm_destroy(VirtualMachine *vm);

ObjString *vm_intern_string(VirtualMachine *vm, StringView str);

void vm_track_object(VirtualMachine *vm, Object *object);

void vm_define_global(VirtualMachine *vm, StringView name, Value value);

void vm_undefine_global(VirtualMachine *vm, StringView name);

void vm_define_native(VirtualMachine *vm, StringView name, NativeFunc function);

ObjImpl //
    *vm_impl_native(VirtualMachine *vm, ReservedDefID def_id, ObjTraitDef *trait,
                    NativeMethodDef *methods, int method_count);

ObjImpl //
    *vm_impl_native_inherent(VirtualMachine *vm, ReservedDefID subject_id, NativeMethodDef *methods,
                             int method_count);

Object *vm_cast(VirtualMachine *vm, Value val, Object *target);

void vm_panic(VirtualMachine *vm, const char *format, ...) __attribute__((noreturn));

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);