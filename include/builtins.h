#pragma once

#include "object.h"

typedef struct {
  ObjTraitDef *iterator;
  ObjTraitDef *iterable;

  ObjImpl *impl_iterable_for_obj_array;
  ObjImpl *impl_iterator_for_obj_array_iter;
} BuiltinRegistry;

bool builtin_register(BuiltinRegistry *registry, VirtualMachine *vm);