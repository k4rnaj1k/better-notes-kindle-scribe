#pragma once
#include "strokes.h"

#include <cairo/cairo.h>

namespace bn {

// Render a finished page: template underlay + every stored stroke.
void canvas_render_page(cairo_t *cr, const Page &p, double w, double h);

// Render a single stroke (used for live preview and replay).
void canvas_render_stroke(cairo_t *cr, const Stroke &s);

// In-progress live preview: same as render_stroke but renders only the
// last few segments to keep the cost flat as a stroke grows.
void canvas_render_stroke_live(cairo_t *cr, const Stroke &s, size_t from_idx);

// Geometric erase: drop strokes whose segments come within radius of the
// (x,y) point. Returns the number of strokes removed.
int canvas_erase_at(Page &p, double x, double y, double radius);

} // namespace bn
