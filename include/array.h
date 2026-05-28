#pragma once

#include "allocator.h"

#define ARRAY_GROW(type, ptr, old_size, new_size, al)                          \
  ptr ? al_realloc(al, ptr, old_size * sizeof(type), new_size * sizeof(type))  \
      : al_alloc(al, new_size * sizeof(type))
