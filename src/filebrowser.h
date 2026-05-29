#pragma once
#include "index.h"

#include <cairo/cairo.h>
#include <functional>
#include <string>

namespace bn {

enum class BrowserAction {
    None,
    Open,
    OpenParent,    // tap on ".." tile, go up one directory
    NewNote,
    NewMarkdown,
    NewFolder,     // create a subfolder in the current directory
    Rename,        // rename the entry at entry_index
    Delete,        // delete the entry at entry_index
    Move,          // start moving the entry at entry_index
    MoveHere,      // drop the in-flight move into the current directory
    MoveCancel,    // abort the in-flight move
};

struct BrowserHit {
    BrowserAction action = BrowserAction::None;
    int           entry_index = -1;   // valid for Open/Rename/Delete/Move
};

class FileBrowser {
public:
    void layout(double w, double h, size_t entry_count);
    void draw(cairo_t *cr, const NotesIndex &idx, double w, double h,
              double scroll, bool move_mode, const std::string &move_title);
    BrowserHit hit(double x, double y, const NotesIndex &idx,
                   double scroll, bool move_mode) const;

    // Total laid-out content height, for clamping the scroll offset.
    double content_height(const NotesIndex &idx) const;

    void set_current_path(const std::string &p) { current_path_ = p; }

private:
    // Grid geometry, derived from the current width/height.
    struct Grid {
        double top = 0, margin = 0, gap = 0, tile_w = 0, tile_h = 0;
        int    cols = 1;
    };
    Grid grid_metrics() const;
    // Rect of the cell at index `cell` (0-based across the grid, including the
    // optional ".." tile at index 0 when below the vault root), given scroll.
    Rect cell_rect(const Grid &g, int cell, double scroll) const;
    int  cell_count(const NotesIndex &idx) const;
    bool has_parent() const { return !current_path_.empty(); }

    double w_ = 0, h_ = 0;
    double header_h_ = 120.0;
    std::string current_path_;  // vault-relative dir we're showing
};

} // namespace bn
