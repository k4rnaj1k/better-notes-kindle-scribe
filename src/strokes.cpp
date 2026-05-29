#include "strokes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bn {

namespace {

// Indexed by PenType. grey/alpha/scale tuned for the greyscale e-ink panel:
// the "grey" pens read as light dots/tints, marker is solid thick black.
const PenStyle kStyles[] = {
    /* Pen         */ { 0.00, 1.00, 1.0, false, false, false, false, false },
    /* Pencil      */ { 0.32, 1.00, 0.9, true,  false, true,  false, false },
    /* Fountain    */ { 0.00, 1.00, 1.3, false, true,  false, false, false },
    /* Marker      */ { 0.00, 1.00, 2.4, false, false, false, false, false },
    /* Highlighter */ { 0.62, 0.45, 5.0, false, false, false, false, true  },
    /* Spray       */ { 0.00, 1.00, 2.0, false, false, false, true,  false },
};

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Shared width modulation so the cairo renderer and the live path agree.
double modulate_width(const PenStyle &ps, double base, double pressure,
                      double dirx, double diry) {
    double w = base;
    if (ps.taper) w *= 0.55 + 0.45 * clamp01(pressure);
    if (ps.directional) {
        double len = std::hypot(dirx, diry);
        if (len > 1e-6) {
            double ang = std::atan2(diry, dirx);
            // Calligraphy nib at 45°: thin along the nib, thick across it.
            double f = std::fabs(std::sin(ang - M_PI / 4.0));
            w *= 0.25 + 0.75 * f;
        }
    }
    return w;
}

} // namespace

const PenStyle &pen_style(PenType t) {
    int i = (int)t;
    if (i < 0 || i >= (int)(sizeof(kStyles) / sizeof(kStyles[0]))) i = 0;
    return kStyles[i];
}

const char *pen_type_name(PenType t) {
    switch (t) {
        case PenType::Pen:         return "pen";
        case PenType::Pencil:      return "pencil";
        case PenType::Fountain:    return "fountain";
        case PenType::Marker:      return "marker";
        case PenType::Highlighter: return "highlighter";
        case PenType::Spray:       return "spray";
    }
    return "pen";
}

PenType pen_type_from_name(const std::string &n) {
    if (n == "pencil")      return PenType::Pencil;
    if (n == "fountain")    return PenType::Fountain;
    if (n == "marker")      return PenType::Marker;
    if (n == "highlighter") return PenType::Highlighter;
    if (n == "spray")       return PenType::Spray;
    return PenType::Pen;
}

double pen_width_at(const Stroke &s, size_t i) {
    const PenStyle &ps = pen_style(s.pen_type);
    double dx = 0, dy = 0;
    if (s.pts.size() >= 2) {
        size_t a = (i == 0) ? 0 : i - 1;
        size_t b = (i == 0) ? 1 : i;
        if (b < s.pts.size()) { dx = s.pts[b].x - s.pts[a].x; dy = s.pts[b].y - s.pts[a].y; }
    }
    double pr = (i < s.pts.size()) ? s.pts[i].pressure : 1.0;
    return modulate_width(ps, s.width, pr, dx, dy);
}

double pen_live_width(const Stroke &s, const Point &prev, const Point &cur) {
    const PenStyle &ps = pen_style(s.pen_type);
    return modulate_width(ps, s.width, cur.pressure, cur.x - prev.x, cur.y - prev.y);
}

Rect Stroke::bbox() const {
    if (pts.empty()) return {};
    double minx =  std::numeric_limits<double>::infinity();
    double miny =  std::numeric_limits<double>::infinity();
    double maxx = -std::numeric_limits<double>::infinity();
    double maxy = -std::numeric_limits<double>::infinity();
    for (auto &p : pts) {
        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
    }
    return { minx - width, miny - width,
             (maxx - minx) + 2*width, (maxy - miny) + 2*width };
}

const char *template_name(TemplateId t) {
    switch (t) {
        case TemplateId::Blank: return "blank";
        case TemplateId::Ruled: return "ruled";
        case TemplateId::Grid:  return "grid";
        case TemplateId::Dot:   return "dot";
    }
    return "blank";
}

TemplateId template_from_name(const std::string &n) {
    if (n == "ruled") return TemplateId::Ruled;
    if (n == "grid")  return TemplateId::Grid;
    if (n == "dot")   return TemplateId::Dot;
    return TemplateId::Blank;
}

} // namespace bn
