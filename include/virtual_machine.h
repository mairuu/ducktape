#pragma once

#include "allocator.h"
#include "garbage_collector.h"
#include "hash_table.h"
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

  uint8_t *ip;
  Value *base;
} CallFrame;

typedef struct VirtualMachine {
  Value *stack_top;
  Value stack[VM_STACK_MAX];
  int frame_count;
  CallFrame frames[VM_FRAMES_MAX];

  Object *objects;
  HashTable strings;
  HashTable globals;

  jmp_buf panic_jump;
  char panic_message[256];

  int gc_disabled;
  GarbageCollector gc;
  Allocator al; // gc allocator

  Allocator inner_al;
} VirtualMachine;

void vm_init(VirtualMachine *vm, Allocator al);

void vm_destroy(VirtualMachine *vm);

ObjString *vm_intern_string(VirtualMachine *vm, StringView str);

void vm_track_object(VirtualMachine *vm, Object *object);

void vm_define_native(VirtualMachine *vm, StringView name, NativeFunc function);

void vm_panic(VirtualMachine *vm, const char *format, ...) __attribute__((noreturn));

void vm_begin_staging(VirtualMachine *vm);

void vm_end_staging(VirtualMachine *vm);

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);