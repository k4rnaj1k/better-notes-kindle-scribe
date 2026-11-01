#include "toolbar.h"

#include "ui.h"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bn {

namespace {

// Draw a recognisable icon for `action`, centred at (cx, cy) with a
// half-extent of `s`. Stroke-based so it renders crisply on e-ink and never
// depends on font glyphs (the Kindle's old Pango drops a lot of symbols).
// The caller sets the source colour beforehand.
void draw_icon(cairo_t *cr, ToolbarAction action,
               double cx, double cy, double s) {
    double lw = std::max(2.0, s * 0.22);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (action) {
    case ToolbarAction::Pen: {
        // Diagonal pen with a nib at the lower-left.
        cairo_move_to(cr, cx - s, cy + s);
        cairo_line_to(cr, cx + s * 0.6, cy - s * 0.8);
        cairo_stroke(cr);
        // nib triangle
        cairo_move_to(cr, cx - s, cy + s);
        cairo_line_to(cr, cx - s * 0.5, cy + s * 0.55);
        cairo_line_to(cr, cx - s * 0.85, cy + s * 0.2);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    }
    case ToolbarAction::Eraser: {
        // Tilted eraser block.
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, -0.5);
        cairo_rectangle(cr, -s, -s * 0.55, 2 * s, s * 1.1);
        cairo_stroke(cr);
        cairo_move_to(cr, 0, -s * 0.55);
        cairo_line_to(cr, 0, s * 0.55);
        cairo_stroke(cr);
        cairo_restore(cr);
        break;
    }
    case ToolbarAction::Lasso: {
        // Dashed loop with a little tail.
        double dashes[] = {lw * 1.4, lw * 1.2};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_save(cr);
        cairo_translate(cr, cx, cy - s * 0.2);
        cairo_scale(cr, 1.0, 0.8);
        cairo_arc(cr, 0, 0, s, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        cairo_move_to(cr, cx - s * 0.3, cy + s * 0.55);
        cairo_line_to(cr, cx - s * 0.1, cy + s);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Keyboard: {
        cairo_rectangle(cr, cx - s, cy - s * 0.65, 2 * s, s * 1.3);
        cairo_stroke(cr);
        // key dots
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 4; ++col) {
                double kx = cx - s * 0.7 + col * (s * 0.46);
                double ky = cy - s * 0.25 + row * (s * 0.5);
                cairo_arc(cr, kx, ky, lw * 0.45, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
        break;
    }
    case ToolbarAction::OcrToggle: {
        // Magnifier over a text baseline = "recognise".
        cairo_arc(cr, cx - s * 0.15, cy - s * 0.15, s * 0.7, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, cx + s * 0.45, cy + s * 0.45);
        cairo_line_to(cr, cx + s, cy + s);
        cairo_stroke(cr);
        // little "A"
        cairo_set_line_width(cr, lw * 0.7);
        cairo_move_to(cr, cx - s * 0.45, cy + s * 0.15);
        cairo_line_to(cr, cx - s * 0.15, cy - s * 0.5);
        cairo_line_to(cr, cx + s * 0.15, cy + s * 0.15);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Undo: {
        // Counter-clockwise arrow.
        cairo_arc(cr, cx, cy, s * 0.8, M_PI * 0.6, M_PI * 1.9);
        cairo_stroke(cr);
        double ax = cx + s * 0.8 * std::cos(M_PI * 0.6);
        double ay = cy + s * 0.8 * std::sin(M_PI * 0.6);
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, ax - s * 0.1, ay - s * 0.5);
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, ax + s * 0.5, ay - s * 0.05);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Save: {
        // Floppy disk.
        cairo_rectangle(cr, cx - s, cy - s, 2 * s, 2 * s);
        cairo_stroke(cr);
        cairo_rectangle(cr, cx - s * 0.5, cy - s, s, s * 0.7);  // shutter
        cairo_stroke(cr);
        cairo_rectangle(cr, cx - s * 0.6, cy + s * 0.1, s * 1.2, s * 0.7);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::ExportPdf: {
        // Page with a folded corner + down arrow (export).
        cairo_move_to(cr, cx - s * 0.7, cy - s);
        cairo_line_to(cr, cx + s * 0.3, cy - s);
        cairo_line_to(cr, cx + s * 0.7, cy - s * 0.6);
        cairo_line_to(cr, cx + s * 0.7, cy + s);
        cairo_line_to(cr, cx - s * 0.7, cy + s);
        cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - s * 0.2);
        cairo_line_to(cr, cx, cy + s * 0.6);
        cairo_move_to(cr, cx - s * 0.3, cy + s * 0.3);
        cairo_line_to(cr, cx, cy + s * 0.6);
        cairo_line_to(cr, cx + s * 0.3, cy + s * 0.3);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::AddPage: {
        cairo_rectangle(cr, cx - s * 0.7, cy - s, s * 1.4, 2 * s);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - s * 0.4);
        cairo_line_to(cr, cx, cy + s * 0.4);
        cairo_move_to(cr, cx - s * 0.4, cy);
        cairo_line_to(cr, cx + s * 0.4, cy);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::PrevPage: {
        cairo_move_to(cr, cx + s * 0.4, cy - s);
        cairo_line_to(cr, cx - s * 0.5, cy);
        cairo_line_to(cr, cx + s * 0.4, cy + s);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::NextPage: {
        cairo_move_to(cr, cx - s * 0.4, cy - s);
        cairo_line_to(cr, cx + s * 0.5, cy);
        cairo_line_to(cr, cx - s * 0.4, cy + s);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Back: {
        // Left arrow.
        cairo_move_to(cr, cx + s, cy);
        cairo_line_to(cr, cx - s, cy);
        cairo_move_to(cr, cx - s * 0.4, cy - s * 0.5);
        cairo_line_to(cr, cx - s, cy);
        cairo_line_to(cr, cx - s * 0.4, cy + s * 0.5);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Browser: {
        // List rows = notes browser.
        for (int i = -1; i <= 1; ++i) {
            double yy = cy + i * (s * 0.7);
            cairo_arc(cr, cx - s * 0.75, yy, lw * 0.5, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_move_to(cr, cx - s * 0.4, yy);
            cairo_line_to(cr, cx + s, yy);
            cairo_stroke(cr);
        }
        break;
    }
    case ToolbarAction::Exit: {
        // Power symbol.
        cairo_arc(cr, cx, cy + s * 0.1, s * 0.8, -M_PI * 0.35, M_PI * 1.35);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - s);
        cairo_line_to(cr, cx, cy + s * 0.1);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Hide: {
        // Up chevron = collapse.
        cairo_move_to(cr, cx - s, cy + s * 0.4);
        cairo_line_to(cr, cx, cy - s * 0.4);
        cairo_line_to(cr, cx + s, cy + s * 0.4);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::MdView: {
        // Eye = preview/view toggle.
        cairo_move_to(cr, cx - s, cy);
        cairo_curve_to(cr, cx - s * 0.4, cy - s * 0.9,
                           cx + s * 0.4, cy - s * 0.9, cx + s, cy);
        cairo_curve_to(cr, cx + s * 0.4, cy + s * 0.9,
                           cx - s * 0.4, cy + s * 0.9, cx - s, cy);
        cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy, s * 0.32, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    }
    case ToolbarAction::InsertDrawing: {
        // Framed picture with a "mountain" + sun = insert image.
        cairo_rectangle(cr, cx - s, cy - s * 0.8, 2 * s, s * 1.6);
        cairo_stroke(cr);
        cairo_arc(cr, cx + s * 0.4, cy - s * 0.3, s * 0.18, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_move_to(cr, cx - s, cy + s * 0.8);
        cairo_line_to(cr, cx - s * 0.2, cy);
        cairo_line_to(cr, cx + s * 0.3, cy + s * 0.4);
        cairo_line_to(cr, cx + s, cy - s * 0.1);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Tags: {
        // Tag/label with a hole.
        cairo_move_to(cr, cx - s * 0.2, cy - s);
        cairo_line_to(cr, cx + s, cy - s);
        cairo_line_to(cr, cx + s, cy + s * 0.2);
        cairo_line_to(cr, cx - s * 0.2, cy + s);
        cairo_line_to(cr, cx - s, cy - s * 0.4);
        cairo_close_path(cr);
        cairo_stroke(cr);
        cairo_arc(cr, cx + s * 0.45, cy - s * 0.4, lw * 0.6, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    }
    case ToolbarAction::OcrWord: {
        // Pen tip over a baseline + "A" = handwriting → text.
        cairo_move_to(cr, cx - s, cy - s * 0.2);
        cairo_line_to(cr, cx + s * 0.3, cy - s);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - s, cy + s);
        cairo_line_to(cr, cx + s, cy + s);   // baseline
        cairo_stroke(cr);
        cairo_set_line_width(cr, lw * 0.7);
        cairo_move_to(cr, cx + s * 0.1, cy + s * 0.5);
        cairo_line_to(cr, cx + s * 0.45, cy - s * 0.4);
        cairo_line_to(cr, cx + s * 0.8, cy + s * 0.5);
        cairo_stroke(cr);
        break;
    }
    case ToolbarAction::Template: {
        // Page with ruled lines = template chooser.
        cairo_rectangle(cr, cx - s * 0.8, cy - s, s * 1.6, 2 * s);
        cairo_stroke(cr);
        cairo_set_line_width(cr, lw * 0.7);
        for (int i = -1; i <= 1; ++i) {
            double yy = cy + i * (s * 0.55);
            cairo_move_to(cr, cx - s * 0.5, yy);
            cairo_line_to(cr, cx + s * 0.5, yy);
        }
        cairo_stroke(cr);
        break;
    }
    default:
        break;
    }
}

void draw_button(cairo_t *cr, const ToolbarButton &b, bool active) {
    // Borderless icon buttons; the active tool is an inked rounded square
    // with a white icon, a pressed one a light grey square. The square is
    // drawn inside the hit rect so a single-button repaint covers it.
    double x = b.rect.x, y = b.rect.y, w = b.rect.w, h = b.rect.h;
    double side = std::min(w - 6, h - 4);
    Rect sq{x + (w - side) / 2.0, y + (h - side) / 2.0, side, side};
    if (active)         ui_fill_round(cr, sq, theme::kRadiusSm + 2, theme::kInk);
    else if (b.pressed) ui_fill_round(cr, sq, theme::kRadiusSm + 2, theme::kPressed);

    double s = side * 0.24;
    ui_grey(cr, active ? theme::kPaper : theme::kInk);
    draw_icon(cr, b.action, x + w / 2.0, y + h / 2.0, s);
}

} // namespace

void Toolbar::layout(double width, ToolbarMode mode) {
    width_  = width;
    mode_   = mode;
    // Tall toolbar for finger-friendly tap targets. Button *width* is
    // computed to fit every button in one row, so it scales with screen
    // width instead of overflowing. A thin status strip sits at the bottom.
    height_ = 120.0;
    const double m = 8.0;
    const double gap = 6.0;
    const double status_h = 30.0;
    buttons_.clear();

    static const ToolbarAction note_defs[] = {
        ToolbarAction::Pen,
        ToolbarAction::Eraser,
        ToolbarAction::Lasso,
        ToolbarAction::Template,
        ToolbarAction::Tags,
        ToolbarAction::Save,
        ToolbarAction::ExportPdf,
        ToolbarAction::AddPage,
        ToolbarAction::PrevPage,
        ToolbarAction::NextPage,
        ToolbarAction::Back,
        ToolbarAction::Browser,
        ToolbarAction::Exit,
        ToolbarAction::Hide,
    };
    static const ToolbarAction md_defs[] = {
        ToolbarAction::MdView,
        ToolbarAction::InsertDrawing,
        ToolbarAction::OcrWord,
        ToolbarAction::Keyboard,
        ToolbarAction::Tags,
        ToolbarAction::Undo,
        ToolbarAction::Save,
        ToolbarAction::Back,
        ToolbarAction::Browser,
        ToolbarAction::Exit,
        ToolbarAction::Hide,
    };
    const ToolbarAction *defs = (mode == ToolbarMode::Markdown)
                                    ? md_defs : note_defs;
    const int n = (mode == ToolbarMode::Markdown)
                      ? (int)(sizeof(md_defs) / sizeof(md_defs[0]))
                      : (int)(sizeof(note_defs) / sizeof(note_defs[0]));
    double bh = height_ - m - status_h;
    double bw = (width_ - 2 * m - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i) {
        ToolbarButton b{};
        b.label  = "";
        b.action = defs[i];
        b.rect.x = m + i * (bw + gap);
        b.rect.y = m;
        b.rect.w = bw;
        b.rect.h = bh;
        if (b.action == ToolbarAction::Pen) pen_btn_rect_ = b.rect;
        buttons_.push_back(b);
    }
}

namespace {
constexpr double kWidthRowH = 64.0;   // height of the width-chip row
constexpr double kTypeRowH  = 84.0;   // height of each pen-type row
} // namespace

double Toolbar::pen_menu_w() const { return std::max(pen_btn_rect_.w, 400.0); }

double Toolbar::pen_menu_x() const {
    double w = pen_menu_w();
    double x = pen_btn_rect_.x + 4;
    if (x + w > width_ - 12) x = width_ - 12 - w;   // keep the panel on-screen
    if (x < 12) x = 12;
    return x;
}

// The panel is a card hanging 8 px below the toolbar; chips and rows sit
// inside it with 12 px padding.
Rect Toolbar::pen_menu_width_rect(int i) const {
    double inner = pen_menu_w() - 24;
    double cw = inner / kPenWidthCount;
    return Rect{pen_menu_x() + 12 + i * cw, height_ + 20, cw, kWidthRowH};
}

Rect Toolbar::pen_menu_type_rect(int i) const {
    return Rect{pen_menu_x() + 12, height_ + 20 + kWidthRowH + 12 + i * kTypeRowH,
                pen_menu_w() - 24, kTypeRowH};
}

PenMenuHit Toolbar::pen_menu_hit(double x, double y) const {
    PenMenuHit h;
    if (!pen_menu_open_) return h;
    for (int i = 0; i < kPenWidthCount; ++i)
        if (pen_menu_width_rect(i).contains(x, y)) {
            h.kind = PenMenuHit::Kind::Width; h.index = i; return h;
        }
    for (int i = 0; i < kPenTypeCount; ++i)
        if (pen_menu_type_rect(i).contains(x, y)) {
            h.kind = PenMenuHit::Kind::Type; h.index = i; return h;
        }
    return h;
}

double Toolbar::pen_menu_extent() const {
    Rect last = pen_menu_type_rect(kPenTypeCount - 1);
    return last.y + last.h;
}

Rect Toolbar::pen_menu_panel_rect() const {
    // Includes the card's padding and drop shadow, so repainting this rect
    // after the menu closes leaves nothing behind.
    double top = height_;
    return Rect{pen_menu_x() - 8, top, pen_menu_w() + 16,
                pen_menu_extent() - top + 12 + theme::kShadowDy};
}

void Toolbar::draw(cairo_t *cr, const ToolState &st,
                   const std::string &status, int page, int total,
                   const char *badge) {
    if (!visible_) return;   // App draws a "show" tab instead.

    ui_grey(cr, theme::kPaper);
    cairo_rectangle(cr, 0, 0, width_, height_); cairo_fill(cr);
    ui_hline(cr, 0, width_, height_ - 1, theme::kLine, 1.5);

    for (auto &b : buttons_) {
        bool active = false;
        switch (b.action) {
            case ToolbarAction::Pen:       active = st.current == Tool::Pen; break;
            case ToolbarAction::Eraser:    active = st.current == Tool::Eraser; break;
            case ToolbarAction::Lasso:     active = st.current == Tool::Lasso; break;
            case ToolbarAction::Keyboard:  active = st.keyboard_visible; break;
            case ToolbarAction::OcrToggle: active = st.ocr_enabled; break;
            case ToolbarAction::MdView:    active = st.markdown_pretty; break;
            default: break;
        }
        draw_button(cr, b, active);
    }

    // Status strip: current tool, then the page, then the latest message;
    // the keyboard-layout badge sits on the right.
    double sy = height_ - 25;
    char tool[64];
    if (st.current == Tool::Pen)
        std::snprintf(tool, sizeof(tool), "%s %s", pen_display_name(st.pen_type),
                      kPenWidthNames[st.pen_width_idx]);
    else
        std::snprintf(tool, sizeof(tool), "%s", tool_label(st.current));
    int tw, th;
    ui_text_size(cr, tool, "Sans Bold 12", &tw, &th);
    ui_text(cr, 14, sy, tool, "Sans Bold 12", theme::kInk);
    char rest[200];
    if (mode_ == ToolbarMode::Note)
        std::snprintf(rest, sizeof(rest), "·   page %d of %d   %s%s", page + 1,
                      total, status.empty() ? "" : "·   ", status.c_str());
    else
        std::snprintf(rest, sizeof(rest), "%s%s", status.empty() ? "" : "·   ",
                      status.c_str());
    ui_text_ellipsized(cr, 14 + tw + 12, sy, width_ - tw - 140, rest,
                       "Sans 12", theme::kMuted, false);
    if (badge && *badge) {
        double bw = ui_pill_width(cr, badge, "Sans Bold 11");
        ui_pill(cr, Rect{width_ - bw - 14, sy - 1, bw, 20}, badge, false,
                "Sans Bold 11");
    }

    // Pen dropdown, drawn last so it overlays the canvas below: a row of width
    // chips, then one preview row per pen type.
    if (pen_menu_open_) {
        draw_pen_menu(cr, st);
    }
}

namespace {

// A short representative sample of `type` at `ew` width, centred vertically in
// `r`, used as the preview swatch in the pen dropdown.
void draw_pen_sample(cairo_t *cr, PenType type, double ew,
                     double x0, double x1, double cy, bool active) {
    const PenStyle &ps = pen_style(type);
    // On the dark active row, force the swatch light so it stays visible;
    // otherwise honour the pen's own grey/alpha.
    double g = active ? std::max(ps.grey, 0.85) : ps.grey;
    double a = active ? 1.0 : ps.alpha;
    cairo_set_source_rgba(cr, g, g, g, a);

    if (ps.spray) {
        double band = std::max(2.0, ew * 0.5);
        for (int k = 0; k < 60; ++k) {
            double t  = (k * 2654435761u % 1000) / 1000.0;
            double aa = (k * 40503u % 1000) / 1000.0 * 2 * M_PI;
            double rr = std::sqrt((k * 22695477u % 1000) / 1000.0) * band;
            double px = x0 + (x1 - x0) * t + std::cos(aa) * rr;
            double py = cy + std::sin(aa) * rr;
            cairo_arc(cr, px, py, 0.8, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        return;
    }
    cairo_set_line_cap(cr, ps.square_cap ? CAIRO_LINE_CAP_SQUARE
                                         : CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (ps.directional) {
        // Fountain: taper thin→thick→thin across the swatch.
        int seg = 16;
        for (int k = 0; k < seg; ++k) {
            double t  = (double)k / seg;
            double w  = ew * (0.25 + 0.75 * std::sin(t * M_PI));
            cairo_set_line_width(cr, std::max(1.0, w));
            cairo_move_to(cr, x0 + (x1 - x0) * t, cy);
            cairo_line_to(cr, x0 + (x1 - x0) * (t + 1.0 / seg), cy);
            cairo_stroke(cr);
        }
        return;
    }
    cairo_set_line_width(cr, std::max(1.0, ew));
    cairo_move_to(cr, x0, cy);
    cairo_line_to(cr, x1, cy);
    cairo_stroke(cr);
    if (ps.grain) {   // pencil graphite specks
        cairo_set_source_rgba(cr, active ? 0.6 : 0.12,
                              active ? 0.6 : 0.12, active ? 0.6 : 0.12, 0.6);
        for (int k = 0; k < 24; ++k) {
            double px = x0 + (x1 - x0) * ((k * 2654435761u % 1000) / 1000.0);
            double py = cy + ((int)(k * 40503u % 100) - 50) / 100.0 * ew;
            cairo_arc(cr, px, py, 0.7, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

} // namespace

void Toolbar::draw_pen_menu(cairo_t *cr, const ToolState &st) {
    // Floating card under the Pen button.
    Rect last = pen_menu_type_rect(kPenTypeCount - 1);
    Rect card{pen_menu_x(), height_ + 8, pen_menu_w(),
              last.y + last.h + 12 - (height_ + 8)};
    ui_card(cr, card);

    // Width chips: a dot sized to the base width over the size name; the
    // selected one is inked.
    for (int i = 0; i < kPenWidthCount; ++i) {
        Rect r = pen_menu_width_rect(i);
        Rect chip{r.x + 4, r.y + 4, r.w - 8, r.h - 8};
        bool active = (i == st.pen_width_idx);
        ui_fill_round(cr, chip, theme::kRadiusSm, active ? theme::kInk : theme::kSurface);
        double fg = active ? theme::kPaper : theme::kInk;
        ui_grey(cr, fg);
        cairo_arc(cr, chip.x + chip.w / 2.0, chip.y + chip.h * 0.36,
                  std::min(kPenWidths[i] * 0.7 + 2.0, chip.h * 0.2), 0, 2 * M_PI);
        cairo_fill(cr);
        int tw, th; ui_text_size(cr, kPenWidthNames[i], "Sans Bold 13", &tw, &th);
        ui_text(cr, chip.x + (chip.w - tw) / 2.0, chip.y + chip.h - th - 5,
                kPenWidthNames[i], "Sans Bold 13", fg);
    }

    // Pen-type rows: a live sample of the stroke, then the name. Hairlines
    // between rows; the selected pen is an inked row.
    for (int i = 0; i < kPenTypeCount; ++i) {
        Rect r = pen_menu_type_rect(i);
        PenType type = kPenTypeMenu[i];
        bool active = (st.current == Tool::Pen && type == st.pen_type);
        if (active) ui_fill_round(cr, r, theme::kRadiusSm, theme::kInk);
        else if (i > 0) ui_hline(cr, r.x + 12, r.x + r.w - 12, r.y, theme::kLine);

        double cy = r.y + r.h * 0.5;
        double ew = effective_pen_width(type, st.pen_width_idx);
        ew = std::min(ew, r.h * 0.45);   // clamp so a wide highlighter still fits
        draw_pen_sample(cr, type, ew, r.x + 24, r.x + 140, cy, active);

        int tw, th; ui_text_size(cr, pen_display_name(type), "Sans 19", &tw, &th);
        ui_text(cr, r.x + 166, cy - th / 2.0, pen_display_name(type), "Sans 19",
                active ? theme::kPaper : theme::kInk);
        if (active) {
            ui_grey(cr, theme::kPaper);
            ui_icon(cr, Icon::Check, r.x + r.w - 32, cy, 12);
        }
    }
}

ToolbarAction Toolbar::hit(double x, double y) const {
    if (!visible_) return ToolbarAction::None;
    for (auto &b : buttons_)
        if (b.rect.contains(x, y)) return b.action;
    return ToolbarAction::None;
}

Rect Toolbar::button_rect_at(double x, double y) const {
    if (visible_)
        for (auto &b : buttons_)
            if (b.rect.contains(x, y)) return b.rect;
    return Rect{0, 0, 0, 0};
}

void Toolbar::press(double x, double y) {
    if (!visible_) return;
    for (auto &b : buttons_) b.pressed = b.rect.contains(x, y);
}

void Toolbar::release_all() {
    for (auto &b : buttons_) b.pressed = false;
}

} // namespace bn
