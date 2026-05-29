#include "canvas.h"
#include "templates.h"
#include "tools.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
// diameter is `2*radius + 1` px. Rendering reloaded strokes at that same width
// makes a plain pen look identical before and after a page reload.
double brush_width(double w) {
    int radius = std::max(1, (int)std::lround(w * 0.5));
    return 2.0 * radius + 1.0;
}

// Deterministic pseudo-random in [0,1) from integer coords + salt. Used for the
// pencil grain and spray scatter so a stroke renders identically every reload
// (a non-deterministic rand() would make the texture shimmer on each repaint).
double hash01(int64_t a, int64_t b, int64_t salt) {
    uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= (uint64_t)salt * 0x100000001B3ull;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (double)(h & 0xFFFFFFFFull) / 4294967296.0;
}

// Solid constant-width polyline (Pen / Marker / Highlighter). One stroked path
// so an alpha (highlighter) stays uniform across bends instead of darkening.
void render_solid(cairo_t *cr, const Stroke &s, const PenStyle &ps) {
    double bw = brush_width(s.width);
    cairo_set_source_rgba(cr, ps.grey, ps.grey, ps.grey, ps.alpha);
    cairo_set_line_cap(cr, ps.square_cap ? CAIRO_LINE_CAP_SQUARE
                                         : CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, bw);
    if (s.pts.size() == 1) {
        cairo_arc(cr, s.pts[0].x, s.pts[0].y, bw * 0.5, 0, 2 * M_PI);
        cairo_fill(cr);
        return;
    }
    cairo_move_to(cr, s.pts[0].x, s.pts[0].y);
    for (size_t j = 1; j < s.pts.size(); ++j)
        cairo_line_to(cr, s.pts[j].x, s.pts[j].y);
    cairo_stroke(cr);
}

// Variable-width body (Pencil taper / Fountain nib): a round disc at every
// point plus a filled quad between consecutive points, so the width can change
// smoothly along the stroke. Every disc and quad is appended to one path and
// filled once — quads are wound the same way as the discs (so the WINDING fill
// rule unions them without holes), and the body is opaque so the single fill
// matches per-shape fills exactly.
void render_variable(cairo_t *cr, const Stroke &s, const PenStyle &ps) {
    cairo_set_source_rgba(cr, ps.grey, ps.grey, ps.grey, ps.alpha);
    size_t n = s.pts.size();
    if (n == 1) {
        double r = std::max(0.5, pen_width_at(s, 0) * 0.5);
        cairo_arc(cr, s.pts[0].x, s.pts[0].y, r, 0, 2 * M_PI);
        cairo_fill(cr);
        return;
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        double x0 = s.pts[i].x, y0 = s.pts[i].y;
        double x1 = s.pts[i + 1].x, y1 = s.pts[i + 1].y;
        double r0 = std::max(0.4, pen_width_at(s, i) * 0.5);
        double r1 = std::max(0.4, pen_width_at(s, i + 1) * 0.5);
        cairo_new_sub_path(cr);
        cairo_arc(cr, x0, y0, r0, 0, 2 * M_PI);
        double dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        double nx = -dy / len, ny = dx / len;   // unit normal
        // CCW order so this quad's winding matches cairo_arc's (no cancellation).
        cairo_move_to(cr, x0 + nx * r0, y0 + ny * r0);
        cairo_line_to(cr, x0 - nx * r0, y0 - ny * r0);
        cairo_line_to(cr, x1 - nx * r1, y1 - ny * r1);
        cairo_line_to(cr, x1 + nx * r1, y1 + ny * r1);
        cairo_close_path(cr);
    }
    double rl = std::max(0.4, pen_width_at(s, n - 1) * 0.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, s.pts[n - 1].x, s.pts[n - 1].y, rl, 0, 2 * M_PI);
    cairo_fill(cr);
}

// Graphite speckle overlay for the pencil. All specks are batched into one
// path and filled once — a fill per dot was the slow part on page reload.
void render_grain(cairo_t *cr, const Stroke &s) {
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, 0.5);
    for (size_t i = 0; i < s.pts.size(); ++i) {
        double w = pen_width_at(s, i);
        int dots = std::min(5, 1 + (int)(w * 0.5));
        int64_t kx = (int64_t)std::lround(s.pts[i].x * 4.0);
        int64_t ky = (int64_t)std::lround(s.pts[i].y * 4.0);
        for (int k = 0; k < dots; ++k) {
            double ang = hash01(kx, ky, (int64_t)i * 7 + k) * 2 * M_PI;
            double rad = hash01(ky, kx, (int64_t)i * 13 + k) * (w * 0.5);
            cairo_rectangle(cr, s.pts[i].x + std::cos(ang) * rad - 0.5,
                                s.pts[i].y + std::sin(ang) * rad - 0.5, 1.2, 1.2);
        }
    }
    cairo_fill(cr);
}

// Spray can: dots scattered within a band around the path, denser sampling
// along each segment so a slow drag lays down more ink than a fast one. Dots
// are cheap rectangles batched into a single fill (was a fill per dot).
void render_spray(cairo_t *cr, const Stroke &s, const PenStyle &ps) {
    cairo_set_source_rgba(cr, ps.grey, ps.grey, ps.grey, ps.alpha);
    double band = std::max(2.0, s.width);
    auto spray_at = [&](double cx, double cy, int64_t seed) {
        int dots = std::min(14, 5 + (int)(band * 0.5));
        int64_t kx = (int64_t)std::lround(cx * 4.0);
        int64_t ky = (int64_t)std::lround(cy * 4.0);
        for (int k = 0; k < dots; ++k) {
            double ang = hash01(kx, ky, seed * 131 + k) * 2 * M_PI;
            double rad = std::sqrt(hash01(ky, kx, seed * 257 + k)) * band;
            cairo_rectangle(cr, cx + std::cos(ang) * rad - 0.5,
                                cy + std::sin(ang) * rad - 0.5, 1.2, 1.2);
        }
    };
    if (s.pts.size() == 1) {
        spray_at(s.pts[0].x, s.pts[0].y, 1);
    } else {
        for (size_t i = 0; i + 1 < s.pts.size(); ++i) {
            double x0 = s.pts[i].x, y0 = s.pts[i].y;
            double x1 = s.pts[i + 1].x, y1 = s.pts[i + 1].y;
            double len = std::hypot(x1 - x0, y1 - y0);
            int steps = std::max(1, (int)(len / std::max(2.0, band * 0.6)));
            for (int q = 0; q < steps; ++q) {
                double t = (double)q / steps;
                spray_at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t,
                         (int64_t)i * 1000 + q);
            }
        }
    }
    cairo_fill(cr);
}

// `from` is kept for API compatibility but styled pens always render the whole
// stroke (grain/spray must stay deterministic; alpha must composite once).
void render_stroke_range(cairo_t *cr, const Stroke &s, size_t from) {
    (void)from;
    if (s.pts.empty()) return;
    const PenStyle &ps = pen_style(s.pen_type);
    if (ps.spray) { render_spray(cr, s, ps); return; }
    if (ps.taper || ps.directional) {
        render_variable(cr, s, ps);
        if (ps.grain) render_grain(cr, s);
        return;
    }
    render_solid(cr, s, ps);
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
    // Cull strokes outside the current clip (the exposed rect on a partial
    // redraw). When the clip already covers the whole page — a full redraw, PDF
    // export, or a page snapshot — skip the per-stroke bbox test entirely so it
    // adds no overhead to those paths.
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
    bool full = (cx1 <= 0.5 && cy1 <= 0.5 && cx2 >= w - 0.5 && cy2 >= h - 0.5);
    for (auto &s : p.strokes) {
        if (!full) {
            Rect b = s.bbox();
            if (b.x > cx2 || b.x + b.w < cx1 || b.y > cy2 || b.y + b.h < cy1)
                continue;
        }
        canvas_render_stroke(cr, s);
    }
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
