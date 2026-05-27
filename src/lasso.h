#pragma once
#include "strokes.h"

namespace bn {

// Lasso captures a freehand polygon while in Lasso tool. Provides hit-tests
// against the closed shape so the link picker can decide which strokes /
// rectangle the user actually circled.
struct Lasso {
    std::vector<Point> pts;
    bool closed = false;

    void clear() { pts.clear(); closed = false; }
    Rect bbox() const;
    bool contains(double x, double y) const;
};

} // namespace bn
