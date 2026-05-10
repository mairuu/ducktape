#include "builtins.h"

#include <stdio.h>
#include <time.h>

#include "garbage_collector.h"
#include "object.h"
#include "string_utils.h"
#include "value.h"
#include "virtual_machine.h"

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

static Value native_clock(VirtualMachine *vm, int arg_count, Value *args) {
  (void)args;
  if (arg_count != 0) {
    vm_panic(vm, "clock() takes no arguments");
  }
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value native_print(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  static char buf[1024];
  for (int i = 0; i < arg_count; i++) {
    val_sprintf(buf, sizeof(buf), args[i]);
    printf("%s", buf);
    if (i < arg_count - 1) {
      printf(" ");
    }
  }
  return NIL_VAL;
}

static Value native_println(VirtualMachine *vm, int arg_count, Value *args) {
  native_print(vm, arg_count, args);
  printf("\n");
  return NIL_VAL;
}

static Value native_panic(VirtualMachine *vm, int arg_count, Value *args) {
  (void)vm;
  char buf[1024];
  int offset = 0;
  for (int i = 0; i < arg_count && offset < (int)sizeof(buf) - 1; i++) {
    if (i > 0) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, " ");
    }
    offset += val_sprintf(buf + offset, sizeof(buf) - offset, args[i]);
  }
  vm_panic(vm, "%s", buf);
}

static Value native_array(VirtualMachine *vm, int arg_count, Value *args) {
  (void)args;
  if (arg_count > 1) {
    vm_panic(vm, "array() takes at most one argument");
  }
  Value size_arg = arg_count == 1 ? args[0] : NUMBER_VAL(0);
  if (!IS_NUMBER(size_arg)) {
    vm_panic(vm, "array size must be a number");
  }
  ObjArray *arr = obj_array_new((int)AS_NUMBER(size_arg), &vm->al);
  vm_track_object(vm, &arr->obj);
  return OBJECT_VAL(arr);
}

void builtin_register(VirtualMachine *vm) {
  struct {
    const char *name;
    NativeFunc function;
  } builtin_functions[] = {
      {.name = "clock", .function = native_clock},
      {.name = "print", .function = native_print},
      {.name = "println", .function = native_println},
      {.name = "panic", .function = native_panic},
      {.name = "array", .function = native_array},
  };

  for (size_t i = 0; i < ARRAY_COUNT(builtin_functions); i++) {
    vm_define_native(vm, sv_from_cstr(builtin_functions[i].name), builtin_functions[i].function);
  }
}