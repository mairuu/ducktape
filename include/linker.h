#pragma once

#include "array.h"
#include "definition.h"
#include "module.h"
#include "object.h"

typedef struct {
  ObjFunction *function;
  int code_offset; // where to patch
} PatchSite;

typedef struct {
  int local_index; // index in this compilation's def_reg
  int vm_index;    // -1 = not yet resolved (new definition)
} LocalToVM;

DEFINE_ARRAY(LocalToVM, LocalToVMArray);

DEFINE_ARRAY(PatchSite, PatchSiteArray);

typedef struct {
  PatchSiteArray *sites;
  Allocator *al;
} Linker;

typedef struct {
  Proto *proto;                // still owned here until vm_load consumes it
  LocalToVMArray *local_to_vm; // for patch sites: local index → VM index
  DefinitionArray
      *defs; // new definitions introduced by this module (delta only, excluding prior_defs)
  PatchSiteArray *sites; // patch sites collected during proto walk
} LinkedModule;

void linker_init(Linker *l, Allocator *al);

LinkedModule linker_link(Linker *l, Module *m, const DefinitionArray *prior_defs);

void linked_module_destroy(LinkedModule *lm, Allocator *al);