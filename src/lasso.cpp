#include "lasso.h"

#include <limits>

namespace bn {

Rect Lasso::bbox() const {
    if (pts.empty()) return {};
    double minx =  std::numeric_limits<double>::infinity();
    double miny =  std::numeric_limits<double>::infinity();
    double maxx = -std::numeric_limits<double>::infinity();
    double maxy = -std::numeric_limits<double>::infinity();
    for (auto &p : pts) {
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    return { minx, miny, maxx - minx, maxy - miny };
}

bool Lasso::contains(double x, double y) const {
    // Standard ray-casting point-in-polygon. Implicitly closes the loop
    // by treating pts.back() ↔ pts.front() as a segment.
    bool inside = false;
    size_t n = pts.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point &a = pts[i], &b = pts[j];
        bool intersects = ((a.y > y) != (b.y > y)) &&
            (x < (b.x - a.x) * (y - a.y) / (b.y - a.y + 1e-9) + a.x);
        if (intersects) inside = !inside;
    }
    return inside;
}

} // namespace bn
