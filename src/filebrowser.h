#pragma once
#include "index.h"

#include <cairo/cairo.h>
#include <functional>
#include <string>

namespace bn {

enum class BrowserAction {
    None,
    Open,
    OpenParent,    // tap on ".." row, go up one directory
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

    void set_current_path(const std::string &p) { current_path_ = p; }
private:
    double w_ = 0, h_ = 0;
    double row_h_ = 80.0;
    double header_h_ = 120.0;
    std::string current_path_;  // vault-relative dir we're showing
};

} // namespace bn
