#pragma once
#include "strokes.h"

namespace bn {

// Pen selection is two independent axes: the pen *type* (render behaviour, see
// PenStyle in strokes.h) and a base *width*. The type's width_scale turns the
// selected base width into the effective stroke width, so e.g. one "M" width
// stays sensible whether it drives a fine pen or a wide highlighter.

// Selectable base widths in drawing units.
inline const double kPenWidths[]     = { 1.5, 3.0, 5.0, 8.0, 12.0 };
inline const char  *kPenWidthNames[] = { "XS", "S", "M", "L", "XL" };
constexpr int kPenWidthCount = (int)(sizeof(kPenWidths) / sizeof(kPenWidths[0]));

// Pen types shown in the dropdown, in display order. Add a type by appending
// it here and to the PenType enum + kStyles table in strokes.
inline const PenType kPenTypeMenu[] = {
    PenType::Pen, PenType::Pencil, PenType::Fountain,
    PenType::Marker, PenType::Highlighter, PenType::Spray,
};
constexpr int kPenTypeCount = (int)(sizeof(kPenTypeMenu) / sizeof(kPenTypeMenu[0]));

// Effective base width = selected base width * the type's scale.
inline double effective_pen_width(PenType t, int width_idx) {
    int i = width_idx < 0 ? 0
          : (width_idx >= kPenWidthCount ? kPenWidthCount - 1 : width_idx);
    return kPenWidths[i] * pen_style(t).width_scale;
}

// Human-readable pen label for the status strip, e.g. "Pencil".
const char *pen_display_name(PenType t);

struct ToolState {
    Tool    current          = Tool::Pen;
    PenType pen_type         = PenType::Pen;
    int     pen_width_idx    = 1;      // index into kPenWidths (default "S")
    double  eraser_radius    = 30.0;
    bool    ocr_enabled      = false;
    bool    keyboard_visible = false;
    bool    markdown_pretty  = false;   // markdown screen: pretty vs source view
};

const char *tool_label(Tool t);

} // namespace bn
