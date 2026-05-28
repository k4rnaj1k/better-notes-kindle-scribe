#include "canvas.h"
#include "templates.h"
#include "tools.h"

#include <algorithm>
#include <cmath>

namespace bn {

namespace {

void render_stroke_range(cairo_t *cr, const Stroke &s, size_t from) {
    if (s.pts.size() < 2 || from >= s.pts.size() - 1) {
        // Single dot
        if (s.pts.size() == 1 && from == 0) {
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_arc(cr, s.pts[0].x, s.pts[0].y, s.width * 0.5, 0,
                      2 * M_PI);
            cairo_fill(cr);
        }
        return;
    }
    // Pure black so the final cairo render matches the crisp black the
    // inkfb fast-path draws (was 0.05 grey, which made strokes fade to grey
    // once the on-screen ink settled to the cairo redraw).
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double pmin = pen_type_pressure_min(s.pen_type);

    // Quantized width of the segment starting at point i.
    const double kQuant = 0.5;  // px buckets for batching adjacent segments
    auto seg_w = [&](size_t i) -> double {
        float pa = s.pts[i].pressure;     if (pa <= 0) pa = 1.0f;
        float pb = s.pts[i + 1].pressure; if (pb <= 0) pb = 1.0f;
        double w = s.width * (pmin + (1.0 - pmin) * (double)(pa + pb) * 0.5);
        double wq = std::round(w / kQuant) * kQuant;
        return wq > 0 ? wq : kQuant;
    };

    // cairo_stroke is expensive per call (it builds + rasterises the stroke
    // geometry each time), so coalesce consecutive segments that share a
    // width into one path and stroke once. Constant-width pens collapse to a
    // single path for the whole stroke; the pressure-tapered pencil batches
    // runs of similar pressure. This turns the O(points) stroke calls — the
    // dominant cost when opening notes or flipping pages — into O(width-runs).
    size_t n = s.pts.size();
    size_t i = std::max<size_t>(from, 0);
    while (i + 1 < n) {
        double wq = seg_w(i);
        cairo_set_line_width(cr, wq);
        cairo_move_to(cr, s.pts[i].x, s.pts[i].y);
        cairo_line_to(cr, s.pts[i + 1].x, s.pts[i + 1].y);
        size_t j = i + 1;
        while (j + 1 < n && seg_w(j) == wq) {
            cairo_line_to(cr, s.pts[j + 1].x, s.pts[j + 1].y);
            ++j;
        }
        cairo_stroke(cr);
        i = j;
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

namespace {
// Squared distance from point (px,py) to segment (ax,ay)→(bx,by).
double seg_dist2(double ax, double ay, double bx, double by,
                 double px, double py) {
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double qx = ax + t * dx, qy = ay + t * dy;
    return (px - qx) * (px - qx) + (py - qy) * (py - qy);
}
} // namespace

Rect canvas_erase_at(Page &p, double x0, double y0,
                     double x1, double y1, double radius) {
    double r2 = radius * radius;
    Rect dirty{0, 0, 0, 0};
    bool any = false;

    // Grow `dirty` to include a pad-inflated box around (cx, cy).
    auto expand = [&](double cx, double cy, double pad) {
        double lx = cx - pad, ly = cy - pad, hx = cx + pad, hy = cy + pad;
        if (!any) {
            dirty = Rect{lx, ly, hx - lx, hy - ly};
            any = true;
        } else {
            double ax = std::min(dirty.x, lx), ay = std::min(dirty.y, ly);
            double bx = std::max(dirty.x + dirty.w, hx);
            double by = std::max(dirty.y + dirty.h, hy);
            dirty = Rect{ax, ay, bx - ax, by - ay};
        }
    };

    auto inside = [&](const Point &pt) {
        return seg_dist2(x0, y0, x1, y1, pt.x, pt.y) <= r2;
    };

    // Bounds of the eraser sweep, used for the per-stroke early-out.
    double sminx = std::min(x0, x1), smaxx = std::max(x0, x1);
    double sminy = std::min(y0, y1), smaxy = std::max(y0, y1);

    std::vector<Stroke> out;
    out.reserve(p.strokes.size());

    for (auto &s : p.strokes) {
        Rect b = s.bbox();
        bool near = !(smaxx < b.x - radius || sminx > b.x + b.w + radius ||
                      smaxy < b.y - radius || sminy > b.y + b.h + radius);
        bool hit = false;
        if (near)
            for (auto &pt : s.pts)
                if (inside(pt)) { hit = true; break; }
        if (!hit) { out.push_back(std::move(s)); continue; }

        // Split into runs of points that survive, dropping erased ones. The
        // removed point and the two segments touching it set the dirty bbox.
        double pad = s.width + 2.0;
        Stroke run;
        run.tool = s.tool; run.pen_type = s.pen_type; run.width = s.width;
        auto flush = [&]() {
            if (!run.pts.empty()) { out.push_back(run); run.pts.clear(); }
        };
        for (size_t i = 0; i < s.pts.size(); ++i) {
            if (inside(s.pts[i])) {
                if (i > 0)                expand(s.pts[i-1].x, s.pts[i-1].y, pad);
                expand(s.pts[i].x, s.pts[i].y, pad);
                if (i + 1 < s.pts.size()) expand(s.pts[i+1].x, s.pts[i+1].y, pad);
                flush();
            } else {
                run.pts.push_back(s.pts[i]);
            }
        }
        flush();
    }

    p.strokes = std::move(out);
    return dirty;
}

} // namespace bn
