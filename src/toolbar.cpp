#include "toolbar.h"

#include <pango/pangocairo.h>
#include <cstdio>

namespace bn {

namespace {

void draw_button(cairo_t *cr, const ToolbarButton &b,
                 bool active, int font_size) {
    double x = b.rect.x, y = b.rect.y, w = b.rect.w, h = b.rect.h;
    cairo_set_source_rgba(cr, 0, 0, 0, b.pressed ? 0.05 : 0.12);
    cairo_rectangle(cr, x + 2, y + 2, w, h); cairo_fill(cr);

    if (active)        cairo_set_source_rgb(cr, 0.20, 0.20, 0.20);
    else if (b.pressed) cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    else                cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, x, y, w, h); cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, x, y, w, h); cairo_stroke(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    char fd_buf[40];
    std::snprintf(fd_buf, sizeof(fd_buf), "Sans Bold %d", font_size);
    PangoFontDescription *fd = pango_font_description_from_string(fd_buf);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, b.label, -1);
    int tw, th; pango_layout_get_size(layout, &tw, &th);
    double tx = x + (w - tw / (double)PANGO_SCALE) / 2.0;
    double ty = y + (h - th / (double)PANGO_SCALE) / 2.0;
    cairo_set_source_rgb(cr, active ? 0.95 : 0.1,
                              active ? 0.95 : 0.1,
                              active ? 0.95 : 0.1);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

} // namespace

void Toolbar::layout(double width) {
    width_  = width;
    // Tall toolbar for finger-friendly tap targets. Button *width* is
    // computed to fit every button in one row, so it scales with the
    // (rotated, portrait) screen width instead of overflowing.
    height_ = 120.0;
    const double m = 8.0;
    const double gap = 6.0;
    buttons_.clear();

    struct Def { const char *label; ToolbarAction action; };
    static const Def defs[] = {
        {"\xe2\x9c\x8f Pen", ToolbarAction::Pen},
        {"Erase",            ToolbarAction::Eraser},
        {"Lasso",            ToolbarAction::Lasso},
        {"Kbd",              ToolbarAction::Keyboard},
        {"OCR",              ToolbarAction::OcrToggle},
        {"Undo",             ToolbarAction::Undo},
        {"Save",             ToolbarAction::Save},
        {"PDF",              ToolbarAction::ExportPdf},
        {"+Pg",              ToolbarAction::AddPage},
        {"<",                ToolbarAction::PrevPage},
        {">",                ToolbarAction::NextPage},
        {"\xe2\x86\x90",     ToolbarAction::Back},
        {"Notes",            ToolbarAction::Browser},
        {"Exit",             ToolbarAction::Exit},
    };
    const int n = (int)(sizeof(defs) / sizeof(defs[0]));
    double bh = height_ - 2 * m;
    double bw = (width_ - 2 * m - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i) {
        ToolbarButton b{};
        b.label  = defs[i].label;
        b.action = defs[i].action;
        b.rect.x = m + i * (bw + gap);
        b.rect.y = m;
        b.rect.w = bw;
        b.rect.h = bh;
        buttons_.push_back(b);
    }
}

void Toolbar::draw(cairo_t *cr, const ToolState &st,
                   const std::string &status, int page, int total) {
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
        draw_button(cr, b, active, 15);
    }

    // Status pill: tool name + page n/N
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 14");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s   p%d/%d   %s",
                  tool_label(st.current), page + 1, total,
                  status.c_str());
    pango_layout_set_text(layout, buf, -1);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, 12, height_ + 6);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

ToolbarAction Toolbar::hit(double x, double y) const {
    for (auto &b : buttons_)
        if (b.rect.contains(x, y)) return b.action;
    return ToolbarAction::None;
}

void Toolbar::press(double x, double y) {
    for (auto &b : buttons_) b.pressed = b.rect.contains(x, y);
}

void Toolbar::release_all() {
    for (auto &b : buttons_) b.pressed = false;
}

} // namespace bn
