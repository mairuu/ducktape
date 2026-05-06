#include "definition.h"

#include <assert.h>

#include "string_utils.h"

static const char *def_type_names[] = {
    [DEFTYPE_STRUCT] = "struct",
    [DEFTYPE_TRAIT] = "trait",
};

const char *def_type_name(DefinitionType type) {
  if (type < 0 || type >= sizeof(def_type_names) / sizeof(def_type_names[0])) {
    return "unknown";
  }
  return def_type_names[type];
}

Definition def_copy(const Definition *def, Allocator *al) {
  Definition copy = {
      .type = def->type,
      .name = str_copy(&def->name, al),
  };
  switch (def->type) {
  case DEFTYPE_STRUCT: {
    const StructDef *struct_def = &def->as.struct_def;
    StructDef *copy_struct_def = &copy.as.struct_def;
    copy_struct_def->field_names = arr_new();
    int field_count = arr_count(struct_def->field_names);
    for (int i = 0; i < field_count; i++) {
      arr_push(copy_struct_def->field_names, str_copy(&struct_def->field_names->data[i], al), al);
    }
    break;
  }
  case DEFTYPE_TRAIT: {
    const TraitDef *trait_def = &def->as.trait_def;
    TraitDef *copy_trait_def = &copy.as.trait_def;
    copy_trait_def->methods = arr_new();
    int method_count = arr_count(trait_def->methods);
    for (int i = 0; i < method_count; i++) {
      TraitMethodDef *method_def = &trait_def->methods->data[i];
      TraitMethodDef copy_method_def = {
          .name = str_copy(&method_def->name, al),
          .arity = method_def->arity,
      };
      arr_push(copy_trait_def->methods, copy_method_def, al);
    }
    break;
  }
  default:
    assert(false && "unknown definition type");
  }
  return copy;
}

void def_destroy(Definition *def, Allocator *al) {
  str_destroy(&def->name, al);
  switch (def->type) {
  case DEFTYPE_STRUCT: {
    StructDef *struct_def = &def->as.struct_def;
    int field_count = arr_count(struct_def->field_names);
    for (int i = 0; i < field_count; i++) {
      str_destroy(&struct_def->field_names->data[i], al);
    }
    arr_free(&struct_def->field_names, al);
    break;
  }
  case DEFTYPE_TRAIT: {
    TraitDef *trait_def = &def->as.trait_def;
    int method_count = arr_count(trait_def->methods);
    for (int i = 0; i < method_count; i++) {
      TraitMethodDef *method_def = &trait_def->methods->data[i];
      str_destroy(&method_def->name, al);
    }
    arr_free(&trait_def->methods, al);
    break;
  }
  default:
    assert(false && "unknown definition type");
  }
}

void defreg_init(DefinitionRegistry *reg, Allocator *al) {
  (void)al;
  reg->defs = arr_new();
  reg->entries = NULL;
  reg->count = 0;
  reg->capacity = 0;
}

void defreg_destroy(DefinitionRegistry *reg, Allocator *al) {
  if (reg->entries) {
    al_free(al, reg->entries, sizeof(*(reg->entries)) * reg->capacity);
    reg->entries = NULL;
  }
  int def_count = arr_count(reg->defs);
  for (int i = 0; i < def_count; i++) {
    def_destroy(&reg->defs->data[i], al);
  }
  arr_free(&reg->defs, al);
}

#define INITIAL_CAPACITY 8

#define HIGH_WATER_MARK 0.75

#define LOW_WATER_MARK 0.5

static void _defreg_alloc(DefinitionRegistry *reg, int capacity, Allocator *al) {
  reg->entries = al_alloc(al, sizeof(*(reg->entries)) * capacity);
  for (int i = 0; i < capacity; i++) {
    reg->entries[i].index = -1;
    reg->entries[i].name = sv_null();
  }
  reg->capacity = capacity;
}

static void _defreg_grow(DefinitionRegistry *reg, Allocator *al) {
  int key_count = 0;
  for (int i = 0; i < reg->capacity; i++) {
    if (reg->entries[i].index != -1) {
      key_count++;
    }
  }

  int new_capacity = reg->capacity;
  while (reg->count >= new_capacity * LOW_WATER_MARK) {
    new_capacity *= 2;
  }
  assert(new_capacity > 0);

  DefinitionRegistry new_reg;
  defreg_init(&new_reg, al);
  _defreg_alloc(&new_reg, new_capacity, al);

  // move definitions
  new_reg.defs = reg->defs;
  reg->defs = NULL;

  for (int i = 0; i < reg->capacity; i++) {
    DefinitionEntry *entry = &reg->entries[i];
    if (entry->index == -1) {
      continue;
    }
    assert(defreg_add(&new_reg, reg->defs->data[entry->index], al) &&
           "definition registry should not have duplicate entries");
  }
  defreg_destroy(reg, al);
  assert(new_reg.count == key_count);
  *reg = new_reg;
}

DefinitionEntry *get_or_insert(DefinitionRegistry *reg, StringView name, Allocator *al,
                               bool *out_exists) {
  if (!reg->entries) {
    _defreg_alloc(reg, INITIAL_CAPACITY, al);
  } else if (reg->count >= reg->capacity * HIGH_WATER_MARK) {
    _defreg_grow(reg, al);
  }

  uint32_t hash = sv_hash(name);

  for (int i = 0; i < reg->capacity; i++) {
    uint32_t index = (hash + i) & (reg->capacity - 1);
    DefinitionEntry *entry = &reg->entries[index];

    if (entry->index != -1 && sv_equal(entry->name, name)) {
      *out_exists = true;
      return entry; // found exact match
    }

    if (entry->index == -1) {
      reg->count++;
      entry->name = name;
      *out_exists = false;
      return entry; // found empty slot
    }
  }
  assert(false && "definition registry is full, should have been resized");
  return NULL;
}

bool defreg_add(DefinitionRegistry *reg, Definition def, Allocator *al) {
  bool exists = false;
  DefinitionEntry *entry = get_or_insert(reg, sv_from_str(def.name), al, &exists);
  if (exists) {
    def_destroy(&def, al);
    return false;
  }
  entry->index = arr_count(reg->defs);
  arr_push(reg->defs, def, al);
  return true;
}

Definition *defreg_find(DefinitionRegistry *reg, StringView name) {
  if (!reg->entries) {
    return NULL;
  }

  uint32_t hash = sv_hash(name);

  for (int i = 0; i < reg->capacity; i++) {
    uint32_t index = (hash + i) & (reg->capacity - 1);
    DefinitionEntry *entry = &reg->entries[index];

    if (entry->index == -1) {
      continue;
    }

    if (sv_equal(entry->name, name)) {
      return &reg->defs->data[entry->index];
    }
  }

  return NULL;
}