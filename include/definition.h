#pragma once

#include "array.h"
#include "string_utils.h"

typedef enum {
  DEFTYPE_STRUCT,
  DEFTYPE_TRAIT,
} DefinitionType;

DEFINE_ARRAY(String, StringArray);

typedef struct {
  StringArray *field_names;
} StructDef;

typedef struct Proto Proto;

typedef struct {
  String name;
  int arity; // included self parameter
  bool has_default_impl;
} TraitMethodDef;

DEFINE_ARRAY(TraitMethodDef, TraitMethodDefArray);

typedef struct {
  TraitMethodDefArray *methods;
} TraitDef;

typedef struct {
  DefinitionType type;
  String name;
  int slot; // stable slot
  union {
    StructDef struct_def;
    TraitDef trait_def;
  } as;
} Definition;

const char *def_type_name(DefinitionType type);

// deep copy definition
Definition def_copy(const Definition *def, Allocator *al);

void def_destroy(Definition *def, Allocator *al);

int traitdef_find_method_index(TraitDef *trait, StringView method_name);

// -------------------------------------------------------------------------------

typedef struct {
  StringView name;
  int index;
} DefinitionEntry;

DEFINE_ARRAY(Definition, DefinitionArray);

typedef struct {
  DefinitionArray *defs;
  DefinitionEntry *entries; // for quick lookup of definitions by name
  int count;
  int capacity;
} DefinitionRegistry;

void defreg_init(DefinitionRegistry *reg, Allocator *al);

void defreg_destroy(DefinitionRegistry *reg, Allocator *al);

// add definition to registry, return false if a definition with the same name already exists
// registry takes ownership of the definition
Definition *defreg_add(DefinitionRegistry *reg, Definition def, Allocator *al);

Definition *defreg_find(DefinitionRegistry *reg, StringView name);