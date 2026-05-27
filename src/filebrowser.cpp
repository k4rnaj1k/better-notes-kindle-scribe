#include "filebrowser.h"

#include <pango/pangocairo.h>

namespace bn {

namespace {

void draw_text(cairo_t *cr, double x, double y, const char *t,
               const char *fontspec, double r, double g, double b) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(fontspec);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, t, -1);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

} // namespace

void FileBrowser::layout(double w, double h, size_t) {
    w_ = w; h_ = h;
}

void FileBrowser::draw(cairo_t *cr, const NotesIndex &idx,
                       double w, double h) {
    w_ = w; h_ = h;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    // Header
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, 0, 0, w_, header_h_); cairo_fill(cr);
    draw_text(cr, 16, 18, "BetterNotes", "Sans Bold 28", 0.1, 0.1, 0.1);

    // Buttons: New note (right), New markdown
    double bw = 160, bh = 50, m = 16;
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_rectangle(cr, w_ - m - bw,       m + 4, bw, bh); cairo_fill(cr);
    cairo_rectangle(cr, w_ - m - 2*bw - 10, m + 4, bw, bh); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, w_ - m - bw,       m + 4, bw, bh); cairo_stroke(cr);
    cairo_rectangle(cr, w_ - m - 2*bw - 10, m + 4, bw, bh); cairo_stroke(cr);
    draw_text(cr, w_ - m - bw + 22,       m + 16,
              "+ New note",     "Sans Bold 16", 0.1, 0.1, 0.1);
    draw_text(cr, w_ - m - 2*bw - 10 + 12, m + 16,
              "+ New markdown", "Sans Bold 14", 0.1, 0.1, 0.1);

    // Entry list
    auto &entries = idx.entries();
    double y = header_h_ + 6;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (y + row_h_ > h_) break;
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, 16, y + row_h_);
        cairo_line_to(cr, w_ - 16, y + row_h_);
        cairo_stroke(cr);

        const char *tag = entries[i].is_markdown ? "MD  " : "    ";
        draw_text(cr, 16, y + 12, tag, "Sans Bold 12", 0.4, 0.4, 0.4);
        draw_text(cr, 60, y + 8, entries[i].title.c_str(),
                  "Sans 18", 0.1, 0.1, 0.1);
        y += row_h_;
    }
}

BrowserHit FileBrowser::hit(double x, double y, size_t entry_count) const {
    double bw = 160, bh = 50, m = 16;
    if (y >= m + 4 && y <= m + 4 + bh) {
        if (x >= w_ - m - bw && x <= w_ - m) return {BrowserAction::NewNote, -1};
        if (x >= w_ - m - 2*bw - 10 && x <= w_ - m - bw - 10)
            return {BrowserAction::NewMarkdown, -1};
    }
    if (y < header_h_) return {};
    int row = (int)((y - header_h_ - 6) / row_h_);
    if (row < 0 || row >= (int)entry_count) return {};
    return {BrowserAction::Open, row};
}

} // namespace bn
