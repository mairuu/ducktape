#include "builtins.h"

#include <stdio.h>
#include <time.h>

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

void builtin_register(VirtualMachine *vm) {
  struct {
    const char *name;
    NativeFunc function;
  } builtin_functions[] = {
      {.name = "clock", .function = native_clock},
      {.name = "print", .function = native_print},
      {.name = "println", .function = native_println},
  };

  for (size_t i = 0; i < ARRAY_COUNT(builtin_functions); i++) {
    vm_define_native(vm, sv_from_cstr(builtin_functions[i].name), builtin_functions[i].function);
  }
}