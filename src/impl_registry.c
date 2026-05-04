#include "impl_registry.h"

#include <assert.h>

static inline uint64_t pack_key(uint32_t receiver_id, uint32_t trait_def_id) {
  return ((uint64_t)receiver_id << 24) | (uint64_t)trait_def_id;
}

static inline Value key_to_value(uint64_t key) {
  union {
    uint64_t i;
    double d;
  } u = {.i = key};
  return NUMBER_VAL(u.d);
}

// static inline uint64_t value_to_key(Value value) {
//   union { double d; uint64_t i; } u = {.d = AS_NUMBER(value)};
//   return u.i;
// }

static inline Value impl_to_value(ObjImpl *impl) { return OBJECT_VAL((Object *)impl); }

static inline ObjImpl *value_to_impl(Value value) { return (ObjImpl *)AS_OBJECT(value); }

void impl_registry_init(ImplRegistry *registry) { ht_init(&registry->_table); }

void impl_registry_destroy(ImplRegistry *registry, Allocator *al) {
  ht_destroy(&registry->_table, al);
}

bool impl_registry_put(ImplRegistry *registry, ObjImpl *impl, Allocator *al) {
  assert(impl != NULL);
  assert(impl->subject != NULL);
  assert(impl->trait != NULL);

  uint64_t key = pack_key(impl->subject->flags.def_id, impl->trait->obj.flags.def_id);
  Value key_val = key_to_value(key);

  // conflict check
  if (ht_get(&registry->_table, key_val) != NULL) {
    return false;
  }

  ht_put(&registry->_table, key_val, impl_to_value(impl), al);
  return true;
}

ObjImpl *impl_registry_get(ImplRegistry *registry, Object *receiver, ObjTraitDef *trait) {
  assert(receiver != NULL);
  assert(trait != NULL);

  uint32_t receiver_id = 0;
  switch (receiver->type) {
  case OBJ_STRUCT_INST:
    receiver_id = ((ObjStructInst *)receiver)->def->obj.flags.def_id;
    break;
  default:
    return NULL; // type cannot implement traits
  }

  uint64_t key = pack_key(receiver_id, trait->obj.flags.def_id);
  Value *result = ht_get(&registry->_table, key_to_value(key));
  if (result == NULL) {
    return NULL;
  }

  return value_to_impl(*result);
}

void impl_registry_iter_init(ImplRegistryIterator *iter, ImplRegistry *registry) {
  iter->impl = NULL;
  hti_init(&iter->_iter, &registry->_table);
}

bool impl_registry_iter_next(ImplRegistryIterator *iter) {
  if (!hti_next(&iter->_iter)) {
    iter->impl = NULL;
    return false;
  }

  iter->impl = value_to_impl(*iter->_iter.value);
  return true;
}
