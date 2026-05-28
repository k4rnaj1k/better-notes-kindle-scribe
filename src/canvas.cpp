#include "canvas.h"
#include "templates.h"
#include "tools.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace bn {

namespace {

std::string g_vault_root;

// Loaded custom-template PNGs, keyed by resolved absolute path. Held for the
// process lifetime (a vault has a handful of templates at most), so loading a
// background never re-hits disk after the first page render.
std::map<std::string, cairo_surface_t *> g_bg_cache;

cairo_surface_t *load_bg(const std::string &rel) {
    std::string path = rel;
    // Resolve vault-relative paths; leave absolute paths as-is.
    if (!path.empty() && path[0] != '/' && !g_vault_root.empty())
        path = g_vault_root + "/" + rel;
    auto it = g_bg_cache.find(path);
    if (it != g_bg_cache.end()) return it->second;
    cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
    if (s && cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        s = nullptr;
    }
    g_bg_cache.emplace(path, s);   // cache nullptr too, so we don't retry misses
    return s;
}

// Paint a custom PNG background scaled to fill the page. Returns false when the
// image can't be loaded so the caller can fall back to the built-in template.
bool paint_bg_image(cairo_t *cr, const std::string &rel, double w, double h) {
    cairo_surface_t *img = load_bg(rel);
    if (!img) return false;
    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return false;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_save(cr);
    cairo_scale(cr, w / (double)iw, h / (double)ih);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
}

// Match the inkfb fast-path brush exactly: it plots a filled disc of
// `radius = max(1, round(width/2))` along the segment, so the effective ink
// diameter is `2*radius + 1` px of pure black. Rendering reloaded strokes at
// that same constant width (instead of the old pressure-tapered, sub-pixel
// quantized line) makes ink look identical before and after a page reload —
// no more grey/narrow appearance.
double brush_width(const Stroke &s) {
    int radius = std::max(1, (int)std::lround(s.width * 0.5));
    return 2.0 * radius + 1.0;
}

void render_stroke_range(cairo_t *cr, const Stroke &s, size_t from) {
    double bw = brush_width(s);

    if (s.pts.size() < 2 || from >= s.pts.size() - 1) {
        // Single dot
        if (s.pts.size() == 1 && from == 0) {
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_arc(cr, s.pts[0].x, s.pts[0].y, bw * 0.5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        return;
    }
    // Pure black, constant width, round caps/joins — one stroked polyline for
    // the whole run. cairo_stroke is expensive per call, so building a single
    // path and stroking once keeps page reloads fast.
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, bw);

    size_t n = s.pts.size();
    size_t i = std::max<size_t>(from, 0);
    cairo_move_to(cr, s.pts[i].x, s.pts[i].y);
    for (size_t j = i + 1; j < n; ++j)
        cairo_line_to(cr, s.pts[j].x, s.pts[j].y);
    cairo_stroke(cr);
}

} // namespace

void canvas_render_stroke(cairo_t *cr, const Stroke &s) {
    render_stroke_range(cr, s, 0);
}

void canvas_render_stroke_live(cairo_t *cr, const Stroke &s, size_t from_idx) {
    render_stroke_range(cr, s, from_idx);
}

void canvas_set_vault_root(const std::string &root) { g_vault_root = root; }

void canvas_render_page(cairo_t *cr, const Page &p, double w, double h) {
    if (p.bg_image.empty() || !paint_bg_image(cr, p.bg_image, w, h))
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
