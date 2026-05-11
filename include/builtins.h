#pragma once

#include "object.h"

typedef struct {
  ObjTraitDef *iterator;
  ObjTraitDef *iterable;
} BuiltinRegistry;

bool builtin_register(BuiltinRegistry *registry, VirtualMachine *vm);