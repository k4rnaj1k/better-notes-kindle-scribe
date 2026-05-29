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
    MdView,        // markdown: toggle source ↔ pretty view
    InsertDrawing, // markdown: open the draw-to-attachment modal
    OcrWord,       // markdown: draw a word, OCR it, insert at cursor
    Tags,          // edit tags for the current note/page
    Template,      // note: open the template picker
};

// The toolbar shows a different button set per screen so markdown-only and
// note-only actions don't clutter each other.
enum class ToolbarMode { Note, Markdown };

// Result of hit-testing the pen dropdown: either a width chip or a pen-type
// row was tapped (with its index into kPenWidths / kPenTypeMenu), or neither.
struct PenMenuHit {
    enum class Kind { None, Width, Type } kind = Kind::None;
    int index = -1;
};

struct ToolbarButton {
    Rect           rect;
    const char    *label;
    ToolbarAction  action;
    bool           pressed = false;
};

class Toolbar {
public:
    void layout(double width, ToolbarMode mode = ToolbarMode::Note);
    void draw(cairo_t *cr, const ToolState &st,
              const std::string &status, int page, int total);

    // Returns the action whose rect contains (x, y), or ::None.
    ToolbarAction hit(double x, double y) const;

    // Rect of the button under (x, y), or a zero-size Rect if none. Lets the
    // app repaint just the tapped button for snappy press feedback instead of
    // refreshing the whole strip.
    Rect button_rect_at(double x, double y) const;

    // Pen dropdown: a row of width chips above one row per pen type, dropping
    // below the Pen button.
    void toggle_pen_menu() { pen_menu_open_ = !pen_menu_open_; }
    void close_pen_menu()  { pen_menu_open_ = false; }
    bool pen_menu_open() const { return pen_menu_open_; }
    // What's under (x, y) while the menu is open (width chip / type row / none).
    PenMenuHit pen_menu_hit(double x, double y) const;
    // Bottom edge (drawing-space y) the expanded dropdown reaches — used to
    // size the partial repaint so opening/closing it never needs a full
    // redraw.
    double pen_menu_extent() const;
    // Just the dropdown panel (below the toolbar strip). Repainting this rect
    // instead of the full-width band keeps opening/closing the menu cheap —
    // the page to the side of the narrow panel never re-renders or refreshes.
    Rect pen_menu_panel_rect() const;

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
    // Dropdown geometry: x/width of the panel, then the rect of width chip i
    // and pen-type row i.
    double pen_menu_x() const;
    double pen_menu_w() const;
    Rect   pen_menu_width_rect(int i) const;
    Rect   pen_menu_type_rect(int i) const;
    void   draw_pen_menu(cairo_t *cr, const ToolState &st);

    std::vector<ToolbarButton> buttons_;
    double height_ = 0;
    double width_  = 0;
    bool   visible_ = true;
    bool   pen_menu_open_ = false;
    Rect   pen_btn_rect_{};   // Pen button rect, anchors the dropdown
};

} // namespace bn
