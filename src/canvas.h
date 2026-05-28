#pragma once
#include "strokes.h"

#include <cairo/cairo.h>
#include <string>

namespace bn {

// Vault root used to resolve a page's custom-template `bg_image` (stored
// vault-relative, e.g. "templates/foo.png"). Set once at startup.
void canvas_set_vault_root(const std::string &root);

// Render a finished page: template/background underlay + every stored stroke.
void canvas_render_page(cairo_t *cr, const Page &p, double w, double h);

// Render a single stroke (used for live preview and replay).
void canvas_render_stroke(cairo_t *cr, const Stroke &s);

// In-progress live preview: same as render_stroke but renders only the
// last few segments to keep the cost flat as a stroke grows.
void canvas_render_stroke_live(cairo_t *cr, const Stroke &s, size_t from_idx);

// Partial erase: drop the points within `radius` of the segment (x0,y0)→
// (x1,y1), splitting each affected stroke into the runs that survive on either
// side. Erasing along the whole segment (not just the endpoint) keeps fast
// drags gap-free. Returns the page-space bbox of the removed ink (zero-size
// Rect if nothing changed) so the caller can repaint just that region.
Rect canvas_erase_at(Page &p, double x0, double y0,
                     double x1, double y1, double radius);

} // namespace bn
