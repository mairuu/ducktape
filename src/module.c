#include "module.h"

void module_destroy(Module *m, Allocator *al) {
  if (m->proto != NULL) {
    proto_destroy(m->proto, al);
    al_free_for(al, m->proto);
    m->proto = NULL;
  }
  defreg_destroy(&m->def_reg, al);
}
