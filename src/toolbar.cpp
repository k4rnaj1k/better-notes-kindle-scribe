#include "toolbar.h"

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
    default:
        break;
    }
}

void draw_button(cairo_t *cr, const ToolbarButton &b, bool active) {
    double x = b.rect.x, y = b.rect.y, w = b.rect.w, h = b.rect.h;
    cairo_set_source_rgba(cr, 0, 0, 0, b.pressed ? 0.05 : 0.12);
    cairo_rectangle(cr, x + 2, y + 2, w, h); cairo_fill(cr);

    if (active)         cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    else if (b.pressed) cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    else                cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, x, y, w, h); cairo_stroke(cr);

    // Icon, centred, sized to the smaller button dimension.
    double s = std::min(w, h) * 0.28;
    double col = active ? 0.95 : 0.12;
    cairo_set_source_rgb(cr, col, col, col);
    draw_icon(cr, b.action, x + w / 2.0, y + h / 2.0, s);
}

} // namespace

void Toolbar::layout(double width) {
    width_  = width;
    // Tall toolbar for finger-friendly tap targets. Button *width* is
    // computed to fit every button in one row, so it scales with screen
    // width instead of overflowing. A thin status strip sits at the bottom.
    height_ = 120.0;
    const double m = 8.0;
    const double gap = 6.0;
    const double status_h = 22.0;
    buttons_.clear();

    struct Def { ToolbarAction action; };
    static const Def defs[] = {
        {ToolbarAction::Pen},
        {ToolbarAction::Eraser},
        {ToolbarAction::Lasso},
        {ToolbarAction::Keyboard},
        {ToolbarAction::OcrToggle},
        {ToolbarAction::Undo},
        {ToolbarAction::Save},
        {ToolbarAction::ExportPdf},
        {ToolbarAction::AddPage},
        {ToolbarAction::PrevPage},
        {ToolbarAction::NextPage},
        {ToolbarAction::Back},
        {ToolbarAction::Browser},
        {ToolbarAction::Exit},
        {ToolbarAction::Hide},
    };
    const int n = (int)(sizeof(defs) / sizeof(defs[0]));
    double bh = height_ - m - status_h;
    double bw = (width_ - 2 * m - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i) {
        ToolbarButton b{};
        b.label  = "";
        b.action = defs[i].action;
        b.rect.x = m + i * (bw + gap);
        b.rect.y = m;
        b.rect.w = bw;
        b.rect.h = bh;
        if (b.action == ToolbarAction::Pen) pen_btn_rect_ = b.rect;
        buttons_.push_back(b);
    }
}

Rect Toolbar::pen_menu_item_rect(int i) const {
    double iw = std::max(pen_btn_rect_.w, 240.0);
    double ih = 76.0;
    return Rect{pen_btn_rect_.x, height_ + i * ih, iw, ih};
}

int Toolbar::pen_menu_hit(double x, double y) const {
    if (!pen_menu_open_) return -1;
    for (int i = 0; i < kPenPresetCount; ++i)
        if (pen_menu_item_rect(i).contains(x, y)) return i;
    return -1;
}

double Toolbar::pen_menu_extent() const {
    if (kPenPresetCount == 0) return height_;
    Rect last = pen_menu_item_rect(kPenPresetCount - 1);
    return last.y + last.h;
}

void Toolbar::draw(cairo_t *cr, const ToolState &st,
                   const std::string &status, int page, int total) {
    if (!visible_) return;   // App draws a "show" tab instead.

    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_rectangle(cr, 0, 0, width_, height_); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, height_); cairo_line_to(cr, width_, height_);
    cairo_stroke(cr);

    for (auto &b : buttons_) {
        bool active = false;
        switch (b.action) {
            case ToolbarAction::Pen:       active = st.current == Tool::Pen; break;
            case ToolbarAction::Eraser:    active = st.current == Tool::Eraser; break;
            case ToolbarAction::Lasso:     active = st.current == Tool::Lasso; break;
            case ToolbarAction::Keyboard:  active = st.keyboard_visible; break;
            case ToolbarAction::OcrToggle: active = st.ocr_enabled; break;
            default: break;
        }
        draw_button(cr, b, active);
    }

    // Status strip at the bottom of the toolbar.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 12");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    char buf[128];
    const char *tool_name = (st.current == Tool::Pen)
        ? kPenPresets[st.pen_preset].name
        : tool_label(st.current);
    std::snprintf(buf, sizeof(buf), "%s   p%d/%d   %s",
                  tool_name, page + 1, total,
                  status.c_str());
    pango_layout_set_text(layout, buf, -1);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, 12, height_ - 20);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    // Pen-preset dropdown, drawn last so it overlays the canvas below.
    if (pen_menu_open_) {
        for (int i = 0; i < kPenPresetCount; ++i) {
            Rect r = pen_menu_item_rect(i);
            bool active = (i == st.pen_preset);

            double bg = active ? 0.20 : 0.97;
            cairo_set_source_rgb(cr, bg, bg, bg);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
            cairo_set_line_width(cr, 1.5);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_stroke(cr);

            // Width preview: a short stroke rendered at the preset's width.
            const PenPreset &pp = kPenPresets[i];
            double cy = r.y + r.h * 0.5;
            double fg = active ? 0.95 : 0.12;
            cairo_set_source_rgb(cr, fg, fg, fg);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_width(cr, std::min(pp.width, r.h * 0.4));
            cairo_move_to(cr, r.x + 18, cy);
            cairo_line_to(cr, r.x + 62, cy);
            cairo_stroke(cr);

            PangoLayout *il = pango_cairo_create_layout(cr);
            PangoFontDescription *ifd =
                pango_font_description_from_string("Sans 15");
            pango_layout_set_font_description(il, ifd);
            pango_font_description_free(ifd);
            pango_layout_set_text(il, pp.name, -1);
            int tw, th; pango_layout_get_pixel_size(il, &tw, &th);
            (void)tw;
            cairo_move_to(cr, r.x + 78, cy - th / 2.0);
            pango_cairo_show_layout(cr, il);
            g_object_unref(il);
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
