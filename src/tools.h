#pragma once
#include "strokes.h"

namespace bn {

struct ToolState {
    Tool   current = Tool::Pen;
    double pen_width    = 1.6;
    double eraser_radius = 14.0;
    bool   ocr_enabled  = false;
    bool   keyboard_visible = false;
};

const char *tool_label(Tool t);

} // namespace bn
