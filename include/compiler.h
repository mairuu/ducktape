#pragma once

#include "allocator.h"
#include "definition.h"
#include "module.h"

bool compile(const char *source, const DefinitionArray *prior_defs, Allocator *al, Module *out_module);