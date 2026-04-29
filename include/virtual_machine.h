#pragma once

#include "allocator.h"
#include "chunk.h"
#include "hash_table.h"
#include "object.h"
#include "value.h"

#include <setjmp.h>

#define VM_STACK_MAX 256

typedef struct {
  Value stack[VM_STACK_MAX];
  Value *stack_top;

  Object *objects;
  HashTable strings;
  HashTable globals;

  Chunk chunk;

  jmp_buf panic_jump;
  char panic_message[256];

  Allocator al;
} VirtualMachine;

void vm_init(VirtualMachine *vm, Allocator al);

void vm_destroy(VirtualMachine *vm);

ObjString *vm_intern_string(VirtualMachine *vm, StringView str);

void vm_track_object(VirtualMachine *vm, Object *object);

void vm_panic(VirtualMachine *vm, const char *format, ...) __attribute__((noreturn));

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

InterpretResult vm_interpret(VirtualMachine *vm, const char *source);