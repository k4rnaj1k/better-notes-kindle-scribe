#include "strokes.h"

#include <algorithm>
#include <limits>

namespace bn {

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
