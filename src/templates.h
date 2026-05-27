#pragma once
#include "strokes.h"

#include <cairo/cairo.h>

namespace bn {

void draw_template(cairo_t *cr, double w, double h, TemplateId t);

} // namespace bn
