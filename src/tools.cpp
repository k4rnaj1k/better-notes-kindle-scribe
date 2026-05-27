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

} // namespace bn
