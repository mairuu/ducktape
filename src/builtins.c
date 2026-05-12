#include "builtins.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

#include "garbage_collector.h"
#include "object.h"
#include "string_utils.h"
#include "value.h"
#include "virtual_machine.h"

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef enum {
  BUILTIN_ITERATOR,
  BUILTIN_ITERABLE,
} BuiltinBindTarget;

const char PRELUDE_SOURCE[] = {
#embed "prelude.dt"
    , 0};

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

#define GET_SELF(Obj, self_var, obj_var)                                                           \
  Value self_var = args[-1];                                                                       \
  Obj *obj_var = (Obj *)AS_TRAIT_OBJECT(self_var)->receiver

#define GET_SELF_INHERENT(Obj, self_var, obj_var)                                                  \
  Value self_var = args[-1];                                                                       \
  Obj *obj_var = (Obj *)AS_OBJECT(self_var)

static Value _arr_iter(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_panic(vm, "iter() takes no arguments");
  }
  GET_SELF(ObjArray, self, arr);

  ObjArrayIterator *arr_iter = obj_array_iterator_new(arr, &vm->al);
  vm_track_object(vm, &arr_iter->obj);

  // Object *iter = vm_cast(vm, OBJECT_VAL(arr_iter), &vm->builtins.iterator->obj);
  // assert(iter != NULL && "iter object should implement iterator trait");

  Object *iter =
      &obj_trait_object_new(&arr_iter->obj, vm->builtins.impl_iterator_for_obj_array_iter, &vm->al)
           ->obj;
  vm_track_object(vm, iter);

  return OBJECT_VAL(iter);
}

static bool impl_iterable_for_obj_array(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {.name = "iter", .func = _arr_iter},
  };
  vm->builtins.impl_iterable_for_obj_array = //
      vm_impl_native(vm, DEFID_ARRAY, vm->builtins.iterable, methods, ARRAY_COUNT(methods));

  return vm->builtins.impl_iterable_for_obj_array != NULL;
}

static Value _arr_iter_has_next(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_panic(vm, "has_next() takes no arguments");
  }
  GET_SELF(ObjArrayIterator, self, iter);
  return BOOL_VAL(iter->index < arr_count(iter->array->values));
}

static Value _arr_iter_next(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_panic(vm, "next() takes no arguments");
  }
  GET_SELF(ObjArrayIterator, self, iter);

  if (iter->index >= arr_count(iter->array->values)) {
    vm_panic(vm, "iterator out of bounds");
  }
  return iter->array->values->data[iter->index++];
}

static bool impl_iterator_for_obj_array_iter(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {.name = "has_next", .func = _arr_iter_has_next},
      {.name = "next", .func = _arr_iter_next},
  };
  vm->builtins.impl_iterator_for_obj_array_iter = //
      vm_impl_native(vm, DEFID_ARRAY_ITERATOR, vm->builtins.iterator, methods,
                     ARRAY_COUNT(methods));

  return vm->builtins.impl_iterator_for_obj_array_iter != NULL;
}

static Value _arr_push(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 1)
    vm_panic(vm, "push() takes one argument");
  GET_SELF_INHERENT(ObjArray, self, arr);
  arr_push(arr->values, args[0], &vm->al);
  return NIL_VAL;
}

static Value _arr_pop(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_panic(vm, "pop() takes no arguments");
  }
  GET_SELF_INHERENT(ObjArray, self, arr);
  if (arr_count(arr->values) == 0) {
    vm_panic(vm, "pop() on empty array");
  }
  return arr_pop(arr->values);
}

static Value _arr_len(VirtualMachine *vm, int arg_count, Value *args) {
  if (arg_count != 0) {
    vm_panic(vm, "len() takes no arguments");
  }
  GET_SELF_INHERENT(ObjArray, self, arr);
  return NUMBER_VAL(arr_count(arr->values));
}

static bool impl_inherent_for_obj_array(VirtualMachine *vm) {
  NativeMethodDef methods[] = {
      {.name = "push", .func = _arr_push},
      {.name = "pop", .func = _arr_pop},
      {.name = "len", .func = _arr_len},
  };
  ObjImpl *impl = vm_impl_native_inherent(vm, DEFID_ARRAY, methods, ARRAY_COUNT(methods));
  if (!impl) {
    return false;
  }

  vm->builtins.array_inherent_impl = impl;
  vm->builtins.array->inherent_impl = impl; // wire up for dispatch
  return true;
}

bool builtin_register(VirtualMachine *vm) {
  struct {
    ObjNativeDef **slot_ptr;
    const char *name;
    uint32_t def_id;
    int slot;
  } natives[] = {
      {&vm->builtins.array, "Array", DEFID_ARRAY, SLOT_ARRAY},
  };

  gc_begin_staging(&vm->gc);
  for (size_t i = 0; i < ARRAY_COUNT(natives); i++) {
    ObjString *name = vm_intern_string(vm, sv_from_cstr(natives[i].name));

    ObjNativeDef *def = obj_native_def_new(name, &vm->al);
    def->obj.flags.def_id = natives[i].def_id;
    vm_track_object(vm, &def->obj);

    vm->definition_objects->data[natives[i].slot] = &def->obj;
    vm->definitions->data[natives[i].slot] = (Definition){
        .type = DEFTYPE_NATIVE,
        .name = str_from_cstr(natives[i].name, &vm->al),
        .slot = natives[i].slot,
    };

    *natives[i].slot_ptr = def;
  }
  gc_end_staging(&vm->gc);

  // ----------------------------------------------------------------------------------

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
    NativeFunc fun;
  } builtin_functions[] = {
      {.name = "clock", .fun = native_clock},     {.name = "print", .fun = native_print},
      {.name = "println", .fun = native_println}, {.name = "panic", .fun = native_panic},
      {.name = "array", .fun = native_array},     {.name = "map", .fun = native_map},
  };

  for (size_t i = 0; i < ARRAY_COUNT(builtin_functions); i++) {
    vm_define_native(vm, sv_from_cstr(builtin_functions[i].name), builtin_functions[i].fun);
  }

  // ----------------------------------------------------------------------------------

  struct {
    bool (*impl_func)(VirtualMachine *vm);
    const char *name;
  } builtin_impls[] = {
      {impl_iterable_for_obj_array, "impl Iterable for ObjArray"},
      {impl_iterator_for_obj_array_iter, "impl Iterator for ObjArrayIterator"},
      {impl_inherent_for_obj_array, "inherent impl for ObjArray"},
  };

  for (size_t i = 0; i < ARRAY_COUNT(builtin_impls); i++) {
    if (!builtin_impls[i].impl_func(vm)) {
      fprintf(stderr, "failed to implement builtin '%s'\n", builtin_impls[i].name);
      return false;
    }
  }

  return true;
}