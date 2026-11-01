#pragma once
// The BetterNotes look: a small design system for an e-ink screen, shared by
// the browser, toolbar, keyboard, modals and pickers.
//
// Everything is greyscale. Fills are solid (no gradients: they dither badly on
// e-ink), shapes are rounded, and depth comes from a solid offset shadow. The
// grey levels below are picked to land on distinct panel levels so nothing
// muddies into its neighbour.

#include "util.h"

#include <cairo/cairo.h>
#include <string>

namespace bn {

namespace theme {
// Grey levels (0 = black, 1 = white).
constexpr double kPaper    = 1.00;   // cards, page, keys
constexpr double kCanvas   = 0.95;   // app background behind cards
constexpr double kSurface  = 0.92;   // bars, keyboard tray, secondary buttons
constexpr double kPressed  = 0.80;   // pressed / selected fill
constexpr double kShadow   = 0.72;   // offset shadow under raised things
constexpr double kLine     = 0.78;   // hairlines and dividers
constexpr double kBorder   = 0.55;   // control outlines
constexpr double kMuted    = 0.42;   // secondary text
constexpr double kInk      = 0.08;   // text, primary fill, active state

constexpr double kRadius   = 16.0;   // cards, modals
constexpr double kRadiusSm = 10.0;   // buttons, keys, chips
constexpr double kShadowDy = 4.0;    // how far shadows drop

// Fonts (Pango specs).
constexpr const char *kFontTitle = "Sans Bold 26";
constexpr const char *kFontHead  = "Sans Bold 22";
constexpr const char *kFontBody  = "Sans 18";
constexpr const char *kFontLabel = "Sans 16";
constexpr const char *kFontSmall = "Sans 13";
} // namespace theme

void ui_grey(cairo_t *cr, double g);

// --- shapes -------------------------------------------------------------
void ui_round_rect_path(cairo_t *cr, const Rect &r, double radius);
void ui_fill_round(cairo_t *cr, const Rect &r, double radius, double grey);
void ui_stroke_round(cairo_t *cr, const Rect &r, double radius, double grey,
                     double line_w = 1.5);
// Raised surface: offset shadow, filled body, hairline border.
void ui_card(cairo_t *cr, const Rect &r, double radius = theme::kRadius,
             double fill = theme::kPaper);
void ui_hline(cairo_t *cr, double x0, double x1, double y,
              double grey = theme::kLine, double line_w = 1.0);
// Dim everything behind a modal.
void ui_backdrop(cairo_t *cr, double w, double h);
// Thick rounded outline for keyboard focus / selection (drawn inside r).
void ui_focus_ring(cairo_t *cr, const Rect &r, double radius = theme::kRadius);

// --- text ---------------------------------------------------------------
// Single-line text with its top-left at (x, y).
void ui_text(cairo_t *cr, double x, double y, const std::string &t,
             const char *font, double grey = theme::kInk);
// Word-wrapped text constrained to `width`.
void ui_text_wrapped(cairo_t *cr, double x, double y, double width,
                     const std::string &t, const char *font,
                     double grey = theme::kInk);
// One line, ellipsized to `width`. at_start keeps the tail visible (paths);
// centered aligns it in the box (tile titles).
void ui_text_ellipsized(cairo_t *cr, double x, double y, double width,
                        const std::string &t, const char *font, double grey,
                        bool at_start, bool centered = false);
// Text centred in r (both axes).
void ui_text_centered(cairo_t *cr, const Rect &r, const std::string &t,
                      const char *font, double grey = theme::kInk);
void ui_text_size(cairo_t *cr, const std::string &t, const char *font,
                  int *w, int *h);

// --- icons --------------------------------------------------------------
// Line icons centred at (cx, cy) with half-extent s, in the current grey.
enum class Icon {
    Plus, Gear, Folder, Notebook, Markdown, Up, Pencil, Move, Trash, Close,
    Check, Keyboard, Globe, Bluetooth,
};
void ui_icon(cairo_t *cr, Icon icon, double cx, double cy, double s);

// --- controls -----------------------------------------------------------
enum class Btn {
    Primary,     // black fill, white text — the one main action
    Secondary,   // light fill, outlined
    Ghost,       // no fill until pressed (toolbars, tile actions)
};
void ui_button(cairo_t *cr, const Rect &r, const char *label, Btn style,
               const char *font = theme::kFontLabel, bool pressed = false);
// Back-compat shorthand: primary = Btn::Primary, else Btn::Secondary.
void ui_button(cairo_t *cr, const Rect &r, const char *label, bool primary,
               const char *font = theme::kFontLabel);
// Icon + label button (label may be empty for an icon-only button).
void ui_icon_button(cairo_t *cr, const Rect &r, Icon icon, const char *label,
                    Btn style, const char *font = theme::kFontLabel);
// Small rounded badge, e.g. a layout indicator or a tag.
void ui_pill(cairo_t *cr, const Rect &r, const std::string &text,
             bool filled, const char *font = theme::kFontSmall);
// Width a pill needs for `text`.
double ui_pill_width(cairo_t *cr, const std::string &text,
                     const char *font = theme::kFontSmall);
// On/off switch.
void ui_switch(cairo_t *cr, const Rect &r, bool on);
// Segmented control; `selected` indexes `labels`.
void ui_segmented(cairo_t *cr, const Rect &r, const char *const *labels,
                  int count, int selected);

// Geometry for the centred, scrollable list modals (link picker, template
// picker). Shared by draw and hit-test so the two can never drift apart.
struct ListModal {
    Rect   card;
    Rect   close;           // the close button in the top-right corner
    double row_h    = 72;
    double list_top = 0;
    double list_bot = 0;
    int    rows_visible = 0;

    static ListModal make(double win_w, double win_h,
                          double max_w, double max_h);
    bool  outside(double x, double y) const { return !card.contains(x, y); }
    // Row index under y (relative to the first visible row), or -1.
    int   row_at(double y) const;
    Rect  row_rect(int visible_row) const;
    // Backdrop, card, title, subtitle and close button; the caller draws rows.
    void  draw_frame(cairo_t *cr, double win_w, double win_h,
                     const char *title, const char *subtitle = nullptr) const;
};

} // namespace bn
