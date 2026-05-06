#pragma once

#include "allocator.h"
#include "definition.h"
#include "module.h"

typedef struct {
  int next_slot;
} SlotAllocator;

bool compile(const char *source, const DefinitionArray *prior_defs, SlotAllocator *slot_al,
             Allocator *al, Module *out_module);