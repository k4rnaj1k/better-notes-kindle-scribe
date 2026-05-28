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
    Undo,   // only meaningful in Markdown screen for now
    Exit,   // quit the app
    Hide,   // collapse the toolbar
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

    // Rect of the button under (x, y), or a zero-size Rect if none. Lets the
    // app repaint just the tapped button for snappy press feedback instead of
    // refreshing the whole strip.
    Rect button_rect_at(double x, double y) const;

    // Pen-preset dropdown: a vertical list that drops below the Pen button.
    void toggle_pen_menu() { pen_menu_open_ = !pen_menu_open_; }
    void close_pen_menu()  { pen_menu_open_ = false; }
    bool pen_menu_open() const { return pen_menu_open_; }
    // Preset index under (x, y) while the menu is open, else -1.
    int  pen_menu_hit(double x, double y) const;
    // Bottom edge (drawing-space y) the expanded dropdown reaches — used to
    // size the partial repaint so opening/closing it never needs a full
    // redraw.
    double pen_menu_extent() const;

    // 0 when hidden so callers that gate on `y < toolbar.height()` and the
    // page layout automatically treat the whole screen as canvas.
    double height() const { return visible_ ? height_ : 0.0; }

    // The laid-out height regardless of visibility — used to size the
    // partial-refresh region when toggling the toolbar on/off.
    double full_height() const { return height_; }

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    void press(double x, double y);
    void release_all();

private:
    Rect pen_menu_item_rect(int i) const;

    std::vector<ToolbarButton> buttons_;
    double height_ = 0;
    double width_  = 0;
    bool   visible_ = true;
    bool   pen_menu_open_ = false;
    Rect   pen_btn_rect_{};   // Pen button rect, anchors the dropdown
};

} // namespace bn
