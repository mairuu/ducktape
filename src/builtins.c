#include "builtins.h"

#include <stdio.h>
#include <time.h>

#include "garbage_collector.h"
#include "object.h"
#include "value.h"
#include "virtual_machine.h"

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef enum {
  BUILTIN_ITERATOR,
  BUILTIN_ITERABLE,
} BuiltinBindTarget;

const char *PRELUDE_SOURCE = //
    "trait Iterator {\n"
    "  fun has_next(self);\n"
    "  fun next(self);\n"
    "}\n"
    "trait Iterable {\n"
    "  fun iter(self) => self as Iterator;\n"
    "}\n"
    "{\n"
    "  var bind = __builtin_bind;\n"
    "  bind(__BUILTIN_ITERATOR, Iterator);\n"
    "  bind(__BUILTIN_ITERABLE, Iterable);\n"
    "}\n";


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

static Value native_map(VirtualMachine *vm, int arg_count, Value *args) {
  (void)args;
  if (arg_count != 0) {
    vm_panic(vm, "map() takes no arguments");
  }
  ObjMap *map = obj_map_new(&vm->al);
  vm_track_object(vm, &map->obj);
  return OBJECT_VAL(map);
}

static Value native_bind_builtin(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 2) {
    vm_panic(vm, "__builtin_bind: takes exactly two arguments");
  }
  if (!IS_NUMBER(args[0])) {
    vm_panic(vm, "__builtin_bind: first argument must be a number");
  }

  int target = (int)AS_NUMBER(args[0]);

  switch (target) {
  case BUILTIN_ITERABLE:
    if (!IS_OBJECT(args[1]) || AS_OBJECT(args[1])->type != OBJ_TRAIT_DEF) {
      vm_panic(vm, "__builtin_bind: BUILTIN_ITERABLE expects a trait definition");
    }
    vm->builtins.iterable = AS_TRAIT_DEF(args[1]);
    break;
  case BUILTIN_ITERATOR:
    if (!IS_OBJECT(args[1]) || AS_OBJECT(args[1])->type != OBJ_TRAIT_DEF) {
      vm_panic(vm, "__builtin_bind: BUILTIN_ITERATOR expects a trait definition");
    }
    vm->builtins.iterator = AS_TRAIT_DEF(args[1]);
    break;
  default:
    vm_panic(vm, "__builtin_bind: unknown target %d", target);
  }
  return NIL_VAL;
}

bool builtin_register(BuiltinRegistry *registry, VirtualMachine *vm) {
  (void)registry;
  vm_define_native(vm, sv_from_cstr("__builtin_bind"), native_bind_builtin);

  struct {
    const char *name;
    double value;
  } constants[] = {
      {"__BUILTIN_ITERATOR", BUILTIN_ITERATOR},
      {"__BUILTIN_ITERABLE", BUILTIN_ITERABLE},
  };

  for (size_t i = 0; i < ARRAY_COUNT(constants); i++) {
    vm_define_global(vm, sv_from_cstr(constants[i].name), NUMBER_VAL(constants[i].value));
  }

  InterpretResult result = vm_interpret(vm, PRELUDE_SOURCE);
  if (result != INTERPRET_OK) {
    fprintf(stderr, "failed to load prelude\n");
    return false;
  }

  struct {
    void *ptr;
    const char *name;
  } builtins_check[] = {
      {.ptr = vm->builtins.iterable, .name = "iterable"},
      {.ptr = vm->builtins.iterator, .name = "iterator"},
  };
  for (size_t i = 0; i < ARRAY_COUNT(builtins_check); i++) {
    if (builtins_check[i].ptr == NULL) {
      fprintf(stderr, "builtin '%s' was not bound\n", builtins_check[i].name);
      return false;
    }
  }

  vm_undefine_global(vm, sv_from_cstr("__builtin_bind"));

  for (size_t i = 0; i < ARRAY_COUNT(constants); i++) {
    vm_undefine_global(vm, sv_from_cstr(constants[i].name));
  }

  // ----------------------------------------------------------------------------------

  struct {
    const char *name;
    NativeFunc function;
  } builtin_functions[] = {
      {.name = "clock", .function = native_clock},     {.name = "print", .function = native_print},
      {.name = "println", .function = native_println}, {.name = "panic", .function = native_panic},
      {.name = "array", .function = native_array},     {.name = "map", .function = native_map},
  };

  for (size_t i = 0; i < ARRAY_COUNT(builtin_functions); i++) {
    vm_define_native(vm, sv_from_cstr(builtin_functions[i].name), builtin_functions[i].function);
  }

  return true;
}