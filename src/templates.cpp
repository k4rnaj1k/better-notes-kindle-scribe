#include "templates.h"

namespace bn {

namespace {

void draw_ruled(cairo_t *cr, double w, double h) {
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 0.6);
    const double step = 40.0;
    for (double y = step; y < h; y += step) {
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
    }
    cairo_stroke(cr);
}

void draw_grid(cairo_t *cr, double w, double h) {
    cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
    cairo_set_line_width(cr, 0.5);
    const double step = 32.0;
    for (double y = step; y < h; y += step) {
        cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y);
    }
    for (double x = step; x < w; x += step) {
        cairo_move_to(cr, x, 0); cairo_line_to(cr, x, h);
    }
    cairo_stroke(cr);
}

void draw_dot(cairo_t *cr, double w, double h) {
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    const double step = 30.0;
    const double r = 0.9;
    for (double y = step; y < h; y += step)
        for (double x = step; x < w; x += step) {
            cairo_arc(cr, x, y, r, 0, 2 * 3.14159265358979);
            cairo_fill(cr);
        }
}

} // namespace

void draw_template(cairo_t *cr, double w, double h, TemplateId t) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    switch (t) {
        case TemplateId::Blank: break;
        case TemplateId::Ruled: draw_ruled(cr, w, h); break;
        case TemplateId::Grid:  draw_grid (cr, w, h); break;
        case TemplateId::Dot:   draw_dot  (cr, w, h); break;
    }
}

} // namespace bn
