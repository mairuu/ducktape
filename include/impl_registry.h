#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "allocator.h"
#include "hash_table.h"
#include "object.h"

typedef struct {
  HashTable _table;
} ImplRegistry;

typedef struct {
  ObjImpl *impl; // current entry, valid after a successful impl_registry_iter_next()
  HashTableIterator _iter;
} ImplRegistryIterator;

void impl_registry_init(ImplRegistry *registry);
void impl_registry_destroy(ImplRegistry *registry, Allocator *al);

// returns false and leaves the registry unchanged if a conflicting impl already exists.
bool impl_registry_put(ImplRegistry *registry, ObjImpl *impl, Allocator *al);

// returns null if no impl is registered for (obj, trait).
ObjImpl *impl_registry_get(ImplRegistry *registry, Object *receiver, ObjTraitDef *trait);

void impl_registry_iter_init(ImplRegistryIterator *iter, ImplRegistry *registry);
bool impl_registry_iter_next(ImplRegistryIterator *iter);
