#ifndef BC_DOT_H
#define BC_DOT_H

#include "ir.h"
#include <stdio.h>

// Print the lowered program as a Graphviz graph (`dot -Tsvg`): a circle per
// `give`, a diamond per `take` (its zero branch leaves from the side with a
// small open circle at the tail), a stop sign per `stop`. Fall-through edges
// are weighted so the main flow runs top to bottom.
void ir_print_dot(FILE *out, const IrProgram *prog, const char *title);

#endif // BC_DOT_H
