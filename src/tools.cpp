#include "tools.h"

namespace bn {

const char *tool_label(Tool t) {
    switch (t) {
        case Tool::Pen:    return "Pen";
        case Tool::Eraser: return "Eraser";
        case Tool::Lasso:  return "Lasso";
        case Tool::Hand:   return "Hand";
    }
    return "?";
}

const char *pen_display_name(PenType t) {
    switch (t) {
        case PenType::Pen:         return "Pen";
        case PenType::Pencil:      return "Pencil";
        case PenType::Fountain:    return "Fountain";
        case PenType::Marker:      return "Marker";
        case PenType::Highlighter: return "Highlighter";
        case PenType::Spray:       return "Spray";
    }
    return "Pen";
}

} // namespace bn
