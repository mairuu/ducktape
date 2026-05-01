#pragma once

#include "array.h"
#include "string_utils.h"

typedef enum {
  DEFTYPE_STRUCT,
} DefinitionType;

DEFINE_ARRAY(String, StringArray);

typedef struct {
  StringArray *field_names;
} StructDef;

typedef struct {
  DefinitionType type;
  String name;
  union {
    StructDef struct_def;
  } as;
} Definition;

const char *def_type_name(DefinitionType type);

// deep copy definition
Definition def_copy(const Definition *def, Allocator *al);

void def_destroy(Definition *def, Allocator *al);

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
bool defreg_add(DefinitionRegistry *reg, Definition def, Allocator *al);

DefinitionEntry *defreg_find(DefinitionRegistry *reg, StringView name);