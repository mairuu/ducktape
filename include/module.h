#pragma once

#include "definition.h"
#include "proto.h"

typedef struct {
  Proto *proto;
  DefinitionRegistry def_reg;
} Module;

void module_destroy(Module *module, Allocator *al);