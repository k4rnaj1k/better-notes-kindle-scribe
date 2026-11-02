#include "ui.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace bn {

using namespace theme;

namespace {

PangoLayout *make_layout(cairo_t *cr, const std::string &t, const char *font) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, t.c_str(), -1);
    return l;
}

void show_at(cairo_t *cr, PangoLayout *l, double x, double y, double grey) {
    ui_grey(cr, grey);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

void line(cairo_t *cr, double x0, double y0, double x1, double y1) {
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
}

} // namespace

void ui_grey(cairo_t *cr, double g) { cairo_set_source_rgb(cr, g, g, g); }

// ----------------------------------------------------------------- shapes

void ui_round_rect_path(cairo_t *cr, const Rect &r, double radius) {
    double rad = std::max(0.0, std::min(radius, std::min(r.w, r.h) / 2.0));
    double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, x1 - rad, y1 - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void ui_fill_round(cairo_t *cr, const Rect &r, double radius, double grey) {
    ui_round_rect_path(cr, r, radius);
    ui_grey(cr, grey);
    cairo_fill(cr);
}

void ui_stroke_round(cairo_t *cr, const Rect &r, double radius, double grey,
                     double line_w) {
    // Inset by half the line so the stroke stays inside r (partial repaints
    // of r then always cover it).
    double h = line_w / 2.0;
    ui_round_rect_path(cr, Rect{r.x + h, r.y + h, r.w - line_w, r.h - line_w},
                       radius - h);
    ui_grey(cr, grey);
    cairo_set_line_width(cr, line_w);
    cairo_stroke(cr);
}

void ui_card(cairo_t *cr, const Rect &r, double radius, double fill) {
    ui_fill_round(cr, Rect{r.x + 1, r.y + kShadowDy, r.w, r.h}, radius, kShadow);
    ui_fill_round(cr, r, radius, fill);
    ui_stroke_round(cr, r, radius, kLine, 1.0);
}

void ui_hline(cairo_t *cr, double x0, double x1, double y, double grey,
              double line_w) {
    ui_grey(cr, grey);
    cairo_set_line_width(cr, line_w);
    line(cr, x0, y + 0.5, x1, y + 0.5);
    cairo_stroke(cr);
}

void ui_backdrop(cairo_t *cr, double w, double h) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
}

void ui_focus_ring(cairo_t *cr, const Rect &r, double radius) {
    ui_stroke_round(cr, r, radius, kInk, 4.0);
}

// ------------------------------------------------------------------- text

void ui_text(cairo_t *cr, double x, double y, const std::string &t,
             const char *font, double grey) {
    show_at(cr, make_layout(cr, t, font), x, y, grey);
}

void ui_text_wrapped(cairo_t *cr, double x, double y, double width,
                     const std::string &t, const char *font, double grey) {
    PangoLayout *l = make_layout(cr, t, font);
    pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    show_at(cr, l, x, y, grey);
}

void ui_text_ellipsized(cairo_t *cr, double x, double y, double width,
                        const std::string &t, const char *font, double grey,
                        bool at_start, bool centered) {
    PangoLayout *l = make_layout(cr, t, font);
    pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(l, at_start ? PANGO_ELLIPSIZE_START
                                           : PANGO_ELLIPSIZE_END);
    if (centered) pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    show_at(cr, l, x, y, grey);
}

void ui_text_size(cairo_t *cr, const std::string &t, const char *font,
                  int *w, int *h) {
    PangoLayout *l = make_layout(cr, t, font);
    pango_layout_get_pixel_size(l, w, h);
    g_object_unref(l);
}

void ui_text_centered(cairo_t *cr, const Rect &r, const std::string &t,
                      const char *font, double grey) {
    PangoLayout *l = make_layout(cr, t, font);
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    show_at(cr, l, r.x + (r.w - tw) / 2.0, r.y + (r.h - th) / 2.0, grey);
}

// ------------------------------------------------------------------ icons

void ui_icon(cairo_t *cr, Icon icon, double cx, double cy, double s) {
    double lw = std::max(2.0, s * 0.17);
    cairo_save(cr);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (icon) {
    case Icon::Plus:
        line(cr, cx - s * 0.8, cy, cx + s * 0.8, cy);
        line(cr, cx, cy - s * 0.8, cx, cy + s * 0.8);
        cairo_stroke(cr);
        break;
    case Icon::Gear:
        for (int i = 0; i < 8; ++i) {
            double a = i * M_PI / 4;
            line(cr, cx + std::cos(a) * s * 0.62, cy + std::sin(a) * s * 0.62,
                     cx + std::cos(a) * s * 0.95, cy + std::sin(a) * s * 0.95);
        }
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy, s * 0.6, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy, s * 0.22, 0, 2 * M_PI);
        cairo_stroke(cr);
        break;
    case Icon::Folder:
        cairo_move_to(cr, cx - s,       cy - s * 0.65);
        cairo_line_to(cr, cx - s * 0.25, cy - s * 0.65);
        cairo_line_to(cr, cx - s * 0.05, cy - s * 0.4);
        cairo_line_to(cr, cx + s,       cy - s * 0.4);
        cairo_line_to(cr, cx + s,       cy + s * 0.7);
        cairo_line_to(cr, cx - s,       cy + s * 0.7);
        cairo_close_path(cr);
        cairo_stroke(cr);
        line(cr, cx - s, cy - s * 0.15, cx + s, cy - s * 0.15);
        cairo_stroke(cr);
        break;
    case Icon::Notebook:
        ui_round_rect_path(cr, Rect{cx - s * 0.7, cy - s, s * 1.5, s * 2}, s * 0.15);
        cairo_stroke(cr);
        line(cr, cx - s * 0.35, cy - s, cx - s * 0.35, cy + s);
        cairo_stroke(cr);
        for (int i = -1; i <= 1; ++i) {
            line(cr, cx - s * 0.9, cy + i * s * 0.55, cx - s * 0.55, cy + i * s * 0.55);
        }
        cairo_stroke(cr);
        break;
    case Icon::Markdown:
        ui_round_rect_path(cr, Rect{cx - s, cy - s * 0.65, s * 2, s * 1.3}, s * 0.2);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - s * 0.65, cy + s * 0.35);   // M
        cairo_line_to(cr, cx - s * 0.65, cy - s * 0.35);
        cairo_line_to(cr, cx - s * 0.35, cy);
        cairo_line_to(cr, cx - s * 0.05, cy - s * 0.35);
        cairo_line_to(cr, cx - s * 0.05, cy + s * 0.35);
        cairo_stroke(cr);
        line(cr, cx + s * 0.5, cy - s * 0.35, cx + s * 0.5, cy + s * 0.3);   // ↓
        cairo_move_to(cr, cx + s * 0.28, cy + s * 0.1);
        cairo_line_to(cr, cx + s * 0.5, cy + s * 0.35);
        cairo_line_to(cr, cx + s * 0.72, cy + s * 0.1);
        cairo_stroke(cr);
        break;
    case Icon::Up:
        line(cr, cx, cy + s * 0.8, cx, cy - s * 0.8);
        cairo_move_to(cr, cx - s * 0.6, cy - s * 0.2);
        cairo_line_to(cr, cx, cy - s * 0.8);
        cairo_line_to(cr, cx + s * 0.6, cy - s * 0.2);
        cairo_stroke(cr);
        break;
    case Icon::Pencil:
        cairo_move_to(cr, cx - s * 0.8, cy + s * 0.8);
        cairo_line_to(cr, cx - s * 0.65, cy + s * 0.3);
        cairo_line_to(cr, cx + s * 0.45, cy - s * 0.8);
        cairo_line_to(cr, cx + s * 0.8, cy - s * 0.45);
        cairo_line_to(cr, cx - s * 0.3, cy + s * 0.65);
        cairo_close_path(cr);
        cairo_stroke(cr);
        break;
    case Icon::Move:
        line(cr, cx - s * 0.9, cy, cx + s * 0.3, cy);
        cairo_move_to(cr, cx - s * 0.1, cy - s * 0.4);
        cairo_line_to(cr, cx + s * 0.3, cy);
        cairo_line_to(cr, cx - s * 0.1, cy + s * 0.4);
        cairo_stroke(cr);
        cairo_move_to(cr, cx + s * 0.1, cy - s * 0.8);
        cairo_line_to(cr, cx + s * 0.8, cy - s * 0.8);
        cairo_line_to(cr, cx + s * 0.8, cy + s * 0.8);
        cairo_line_to(cr, cx + s * 0.1, cy + s * 0.8);
        cairo_stroke(cr);
        break;
    case Icon::Trash:
        line(cr, cx - s * 0.85, cy - s * 0.6, cx + s * 0.85, cy - s * 0.6);
        line(cr, cx - s * 0.3, cy - s * 0.6, cx - s * 0.2, cy - s * 0.9);
        line(cr, cx - s * 0.2, cy - s * 0.9, cx + s * 0.2, cy - s * 0.9);
        line(cr, cx + s * 0.2, cy - s * 0.9, cx + s * 0.3, cy - s * 0.6);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - s * 0.65, cy - s * 0.6);
        cairo_line_to(cr, cx - s * 0.5, cy + s * 0.9);
        cairo_line_to(cr, cx + s * 0.5, cy + s * 0.9);
        cairo_line_to(cr, cx + s * 0.65, cy - s * 0.6);
        cairo_stroke(cr);
        line(cr, cx - s * 0.2, cy - s * 0.25, cx - s * 0.15, cy + s * 0.55);
        line(cr, cx + s * 0.2, cy - s * 0.25, cx + s * 0.15, cy + s * 0.55);
        cairo_stroke(cr);
        break;
    case Icon::Close:
        line(cr, cx - s * 0.6, cy - s * 0.6, cx + s * 0.6, cy + s * 0.6);
        line(cr, cx + s * 0.6, cy - s * 0.6, cx - s * 0.6, cy + s * 0.6);
        cairo_stroke(cr);
        break;
    case Icon::Check:
        cairo_move_to(cr, cx - s * 0.7, cy);
        cairo_line_to(cr, cx - s * 0.2, cy + s * 0.5);
        cairo_line_to(cr, cx + s * 0.75, cy - s * 0.55);
        cairo_stroke(cr);
        break;
    case Icon::Keyboard:
        ui_round_rect_path(cr, Rect{cx - s, cy - s * 0.65, s * 2, s * 1.3}, s * 0.2);
        cairo_stroke(cr);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 4; ++col) {
                cairo_arc(cr, cx - s * 0.6 + col * s * 0.4, cy - s * 0.25 + row * s * 0.35,
                          lw * 0.45, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        line(cr, cx - s * 0.45, cy + s * 0.4, cx + s * 0.45, cy + s * 0.4);
        cairo_stroke(cr);
        break;
    case Icon::Bluetooth:   // the rune: a vertical stem with two folded arrows
        cairo_move_to(cr, cx - s * 0.5, cy - s * 0.45);
        cairo_line_to(cr, cx + s * 0.45, cy + s * 0.45);
        cairo_line_to(cr, cx, cy + s * 0.9);
        cairo_line_to(cr, cx, cy - s * 0.9);
        cairo_line_to(cr, cx + s * 0.45, cy - s * 0.45);
        cairo_line_to(cr, cx - s * 0.5, cy + s * 0.45);
        cairo_stroke(cr);
        break;
    case Icon::Power:   // a broken ring with a stem through the gap
        cairo_arc(cr, cx, cy + s * 0.08, s * 0.78, -M_PI / 2 + 0.75, 3 * M_PI / 2 - 0.75);
        cairo_stroke(cr);
        line(cr, cx, cy - s * 0.95, cx, cy - s * 0.1);
        cairo_stroke(cr);
        break;
    case Icon::Globe:
        cairo_arc(cr, cx, cy, s * 0.85, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, 0.4, 1.0);
        cairo_arc(cr, 0, 0, s * 0.85, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
        line(cr, cx - s * 0.85, cy, cx + s * 0.85, cy);
        cairo_stroke(cr);
        break;
    }
    cairo_restore(cr);
}

// --------------------------------------------------------------- controls

namespace {

// Fill + text grey for a button style.
void button_colors(Btn style, bool pressed, double *bg, double *fg, bool *outline) {
    switch (style) {
    case Btn::Primary:
        *bg = pressed ? 0.35 : kInk; *fg = kPaper; *outline = false; break;
    case Btn::Secondary:
        *bg = pressed ? kPressed : kPaper; *fg = kInk; *outline = true; break;
    case Btn::Ghost:
        *bg = pressed ? kPressed : -1; *fg = kInk; *outline = false; break;
    }
}

} // namespace

void ui_button(cairo_t *cr, const Rect &r, const char *label, Btn style,
               const char *font, bool pressed) {
    double bg, fg; bool outline;
    button_colors(style, pressed, &bg, &fg, &outline);
    if (bg >= 0) ui_fill_round(cr, r, kRadiusSm, bg);
    if (outline) ui_stroke_round(cr, r, kRadiusSm, kBorder, 1.5);
    ui_text_centered(cr, r, label, font, fg);
}

void ui_button(cairo_t *cr, const Rect &r, const char *label, bool primary,
               const char *font) {
    ui_button(cr, r, label, primary ? Btn::Primary : Btn::Secondary, font);
}

void ui_icon_button(cairo_t *cr, const Rect &r, Icon icon, const char *label,
                    Btn style, const char *font) {
    double bg, fg; bool outline;
    button_colors(style, false, &bg, &fg, &outline);
    if (bg >= 0) ui_fill_round(cr, r, kRadiusSm, bg);
    if (outline) ui_stroke_round(cr, r, kRadiusSm, kBorder, 1.5);

    double s = std::min(r.h * 0.22, 14.0);
    bool has_label = label && *label;
    int tw = 0, th = 0;
    if (has_label) ui_text_size(cr, label, font, &tw, &th);
    double gap = has_label ? 10.0 : 0.0;
    double total = 2 * s + gap + tw;
    double x = r.x + (r.w - total) / 2.0;
    ui_grey(cr, fg);
    ui_icon(cr, icon, x + s, r.y + r.h / 2.0, s);
    if (has_label) ui_text(cr, x + 2 * s + gap, r.y + (r.h - th) / 2.0, label, font, fg);
}

double ui_pill_width(cairo_t *cr, const std::string &text, const char *font) {
    int tw, th; ui_text_size(cr, text, font, &tw, &th);
    return tw + 24.0;
}

void ui_pill(cairo_t *cr, const Rect &r, const std::string &text, bool filled,
             const char *font) {
    if (filled) ui_fill_round(cr, r, r.h / 2.0, kInk);
    else        ui_stroke_round(cr, r, r.h / 2.0, kBorder, 1.5);
    ui_text_centered(cr, r, text, font, filled ? kPaper : kInk);
}

void ui_switch(cairo_t *cr, const Rect &r, bool on) {
    double rad = r.h / 2.0;
    if (on) ui_fill_round(cr, r, rad, kInk);
    else { ui_fill_round(cr, r, rad, kSurface); ui_stroke_round(cr, r, rad, kBorder, 1.5); }
    double knob = r.h - 10;
    double kx = on ? r.x + r.w - 5 - knob : r.x + 5;
    ui_fill_round(cr, Rect{kx, r.y + 5, knob, knob}, knob / 2.0, kPaper);
    if (!on) ui_stroke_round(cr, Rect{kx, r.y + 5, knob, knob}, knob / 2.0, kBorder, 1.5);
}

void ui_segmented(cairo_t *cr, const Rect &r, const char *const *labels,
                  int count, int selected) {
    ui_fill_round(cr, r, kRadiusSm, kSurface);
    double w = r.w / count;
    for (int i = 0; i < count; ++i) {
        Rect seg{r.x + i * w + 4, r.y + 4, w - 8, r.h - 8};
        if (i == selected) ui_fill_round(cr, seg, kRadiusSm - 3, kInk);
        ui_text_centered(cr, seg, labels[i], "Sans Bold 16",
                         i == selected ? kPaper : kInk);
    }
}

// ------------------------------------------------------------- list modal

ListModal ListModal::make(double win_w, double win_h,
                          double max_w, double max_h) {
    ListModal m;
    double cw = std::min(max_w, win_w * 0.86);
    double ch = std::min(max_h, win_h * 0.80);
    m.card = Rect{(win_w - cw) / 2.0, (win_h - ch) / 2.0, cw, ch};
    const double xs = 60;
    m.close = Rect{m.card.x + cw - xs - 16, m.card.y + 16, xs, xs};
    m.list_top = m.card.y + 100;
    m.list_bot = m.card.y + ch - 20;
    m.rows_visible = std::max(0, (int)((m.list_bot - m.list_top) / m.row_h));
    return m;
}

int ListModal::row_at(double y) const {
    if (y < list_top || y >= list_bot) return -1;
    int r = (int)((y - list_top) / row_h);
    return r < rows_visible ? r : -1;
}

Rect ListModal::row_rect(int visible_row) const {
    return Rect{card.x + 16, list_top + visible_row * row_h,
                card.w - 32, row_h - 6};
}

void ListModal::draw_frame(cairo_t *cr, double win_w, double win_h,
                           const char *title, const char *subtitle) const {
    ui_backdrop(cr, win_w, win_h);
    ui_card(cr, card);
    ui_text(cr, card.x + 28, card.y + 24, title, kFontHead);
    if (subtitle)
        ui_text(cr, card.x + 28, card.y + 62, subtitle, kFontSmall, kMuted);
    ui_fill_round(cr, close, close.h / 2.0, kSurface);
    ui_grey(cr, kInk);
    ui_icon(cr, Icon::Close, close.x + close.w / 2.0, close.y + close.h / 2.0, 16);
    ui_hline(cr, card.x + 16, card.x + card.w - 16, list_top - 8);
}

} // namespace bn
