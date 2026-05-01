#include "linker.h"
#include "module.h"
#include "proto.h"

void linker_init(Linker *l, Allocator *al) { l->al = al; }

static int find_definition(StringView name, const DefinitionArray *prior_defs) {
  if (!prior_defs) {
    return -1;
  }
  int prior_count = arr_count(prior_defs);
  for (int j = 0; j < prior_count; j++) {
    if (sv_equal(name, sv_from_str(prior_defs->data[j].name))) {
      return j;
    }
  }
  return -1;
}

LinkedModule linker_link(Linker *l, Module *m, const DefinitionArray *prior_defs) {
  int def_count = arr_count(m->def_reg.defs);

  LinkedModule lm = {
      .proto = m->proto,
      .defs = arr_new(),
      .local_to_vm = arr_new(),
      .sites = arr_new(),
  };
  m->proto = NULL;

  for (int i = 0; i < def_count; i++) {
    StringView name = sv_from_str(m->def_reg.defs->data[i].name);
    int prior_index = find_definition(name, prior_defs);

    LocalToVM mapping = {.local_index = i};

    if (prior_index != -1) {
      mapping.vm_index = prior_index; // fully resolved now
    } else {
      mapping.vm_index = -1; // resolved in vm_load after append
      arr_push(lm.defs, m->def_reg.defs->data[i], l->al);
      m->def_reg.defs->data[i] = (Definition){0};
    }

    arr_push(lm.local_to_vm, mapping, l->al);
  }

  module_destroy(m, l->al);
  return lm;
}

void linked_module_destroy(LinkedModule *lm, Allocator *al) {
  proto_destroy(lm->proto, al);
  al_free_for(al, lm->proto);
  arr_free(&lm->local_to_vm, al);
  int def_count = arr_count(lm->defs);
  for (int i = 0; i < def_count; i++) {
    def_destroy(&lm->defs->data[i], al);
  }
  arr_free(&lm->defs, al);
  arr_free(&lm->sites, al);
}