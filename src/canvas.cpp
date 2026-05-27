#include "canvas.h"
#include "templates.h"

#include <algorithm>
#include <cmath>

namespace bn {

namespace {

double seg_point_dist2(double ax, double ay, double bx, double by,
                       double px, double py) {
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double qx = ax + t * dx, qy = ay + t * dy;
    return (qx - px) * (qx - px) + (qy - py) * (qy - py);
}

void stroke_segment(cairo_t *cr, const Stroke &s,
                    size_t i, size_t j, double w) {
    cairo_set_line_width(cr, w);
    cairo_move_to(cr, s.pts[i].x, s.pts[i].y);
    cairo_line_to(cr, s.pts[j].x, s.pts[j].y);
    cairo_stroke(cr);
}

void render_stroke_range(cairo_t *cr, const Stroke &s, size_t from) {
    if (s.pts.size() < 2 || from >= s.pts.size() - 1) {
        // Single dot
        if (s.pts.size() == 1 && from == 0) {
            cairo_arc(cr, s.pts[0].x, s.pts[0].y, s.width * 0.5, 0,
                      2 * M_PI);
            cairo_fill(cr);
        }
        return;
    }
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    // Pressure-modulated width: each segment uses average of endpoints.
    for (size_t i = std::max<size_t>(from, 0); i + 1 < s.pts.size(); ++i) {
        float pa = s.pts[i].pressure;
        float pb = s.pts[i + 1].pressure;
        if (pa <= 0) pa = 1.0f;
        if (pb <= 0) pb = 1.0f;
        double w = s.width * (0.4 + 0.6 * (double)(pa + pb) * 0.5);
        stroke_segment(cr, s, i, i + 1, w);
    }
}

} // namespace

void canvas_render_stroke(cairo_t *cr, const Stroke &s) {
    render_stroke_range(cr, s, 0);
}

void canvas_render_stroke_live(cairo_t *cr, const Stroke &s, size_t from_idx) {
    render_stroke_range(cr, s, from_idx);
}

void canvas_render_page(cairo_t *cr, const Page &p, double w, double h) {
    draw_template(cr, w, h, p.tmpl);
    for (auto &s : p.strokes) canvas_render_stroke(cr, s);
}

int canvas_erase_at(Page &p, double x, double y, double radius) {
    int removed = 0;
    double r2 = radius * radius;
    auto hit = [&](const Stroke &s) -> bool {
        Rect b = s.bbox();
        if (x < b.x - radius || x > b.x + b.w + radius ||
            y < b.y - radius || y > b.y + b.h + radius) return false;
        if (s.pts.size() == 1) {
            double dx = s.pts[0].x - x, dy = s.pts[0].y - y;
            return dx*dx + dy*dy <= r2;
        }
        for (size_t i = 0; i + 1 < s.pts.size(); ++i) {
            if (seg_point_dist2(s.pts[i].x,   s.pts[i].y,
                                s.pts[i+1].x, s.pts[i+1].y,
                                x, y) <= r2) return true;
        }
        return false;
    };
    auto it = std::remove_if(p.strokes.begin(), p.strokes.end(),
                             [&](const Stroke &s){ return hit(s); });
    removed = (int)std::distance(it, p.strokes.end());
    p.strokes.erase(it, p.strokes.end());
    return removed;
}

} // namespace bn
