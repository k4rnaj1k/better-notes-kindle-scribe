#pragma once
#include "index.h"

#include <cairo/cairo.h>
#include <functional>
#include <string>

namespace bn {

enum class BrowserAction {
    None,
    Open,
    NewNote,
    NewMarkdown,
};

struct BrowserHit {
    BrowserAction action = BrowserAction::None;
    int           entry_index = -1;   // valid when action == Open
};

class FileBrowser {
public:
    void layout(double w, double h, size_t entry_count);
    void draw(cairo_t *cr, const NotesIndex &idx, double w, double h);
    BrowserHit hit(double x, double y, size_t entry_count) const;
private:
    double w_ = 0, h_ = 0;
    double row_h_ = 56.0;
    double header_h_ = 84.0;
};

} // namespace bn
