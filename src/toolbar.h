#pragma once
#include "tools.h"
#include "util.h"

#include <cairo/cairo.h>
#include <string>
#include <vector>

namespace bn {

enum class ToolbarAction {
    None,
    Pen,
    Eraser,
    Lasso,
    Keyboard,
    OcrToggle,
    Save,
    ExportPdf,
    AddPage,
    PrevPage,
    NextPage,
    Back,
    Browser,
};

struct ToolbarButton {
    Rect           rect;
    const char    *label;
    ToolbarAction  action;
    bool           pressed = false;
};

class Toolbar {
public:
    void layout(double width);                          // computes rects
    void draw(cairo_t *cr, const ToolState &st,
              const std::string &status, int page, int total);

    // Returns the action whose rect contains (x, y), or ::None.
    ToolbarAction hit(double x, double y) const;
    double height() const { return height_; }

    void press(double x, double y);
    void release_all();

private:
    std::vector<ToolbarButton> buttons_;
    double height_ = 0;
    double width_  = 0;
};

} // namespace bn
