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

// Rename / Delete button rects for the row whose top edge is at row_y.
void row_action_rects(double w, double row_y, double row_h,
                      Rect &rename, Rect &del) {
    const double bw = 96, bh = 52, gap = 10, m = 16;
    double by = row_y + (row_h - bh) / 2.0;
    del    = Rect{w - m - bw,            by, bw, bh};
    rename = Rect{w - m - 2 * bw - gap,  by, bw, bh};
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
    draw_text(cr, 20, 26, "BetterNotes", "Sans Bold 32", 0.1, 0.1, 0.1);

    // Breadcrumb path (relative to vault root) under the title
    if (!current_path_.empty()) {
        std::string crumb = "/" + current_path_;
        draw_text(cr, 20, 70, crumb.c_str(), "Sans 16", 0.35, 0.35, 0.35);
    }

    // Buttons: New note (right), New markdown
    double bw = 210, bh = 80, m = 16;
    double btn_y = (header_h_ - bh) / 2.0;
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_rectangle(cr, w_ - m - bw,           btn_y, bw, bh); cairo_fill(cr);
    cairo_rectangle(cr, w_ - m - 2*bw - 10,    btn_y, bw, bh); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, w_ - m - bw,           btn_y, bw, bh); cairo_stroke(cr);
    cairo_rectangle(cr, w_ - m - 2*bw - 10,    btn_y, bw, bh); cairo_stroke(cr);
    draw_text(cr, w_ - m - bw + 26,            btn_y + 26,
              "+ New note",     "Sans Bold 20", 0.1, 0.1, 0.1);
    draw_text(cr, w_ - m - 2*bw - 10 + 14,     btn_y + 26,
              "+ New markdown", "Sans Bold 18", 0.1, 0.1, 0.1);

    // Entry list — parent-folder row, then folders, then files
    auto &entries = idx.entries();
    double y = header_h_ + 6;

    // Parent ".." row when we're below the vault root
    if (!current_path_.empty()) {
        if (y + row_h_ <= h_) {
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
            cairo_set_line_width(cr, 0.5);
            cairo_move_to(cr, 16, y + row_h_);
            cairo_line_to(cr, w_ - 16, y + row_h_);
            cairo_stroke(cr);
            draw_text(cr, 20, y + 20, "DIR ",
                      "Sans Bold 14", 0.4, 0.4, 0.4);
            draw_text(cr, 70, y + 14, "..   (parent)",
                      "Sans 22", 0.1, 0.1, 0.1);
            y += row_h_;
        }
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (y + row_h_ > h_) break;
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, 16, y + row_h_);
        cairo_line_to(cr, w_ - 16, y + row_h_);
        cairo_stroke(cr);

        const char *tag = entries[i].is_folder    ? "DIR "
                        : entries[i].is_markdown  ? "MD  "
                        :                            "NOTE";
        draw_text(cr, 20, y + 20, tag, "Sans Bold 14", 0.4, 0.4, 0.4);
        draw_text(cr, 70, y + 14, entries[i].title.c_str(),
                  "Sans 22", 0.1, 0.1, 0.1);

        // Per-row Rename / Delete buttons (notes + markdown only).
        if (!entries[i].is_folder) {
            Rect rn, dl;
            row_action_rects(w_, y, row_h_, rn, dl);
            for (int b = 0; b < 2; ++b) {
                const Rect &br = b == 0 ? rn : dl;
                cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
                cairo_rectangle(cr, br.x, br.y, br.w, br.h); cairo_fill(cr);
                cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
                cairo_set_line_width(cr, 1.5);
                cairo_rectangle(cr, br.x, br.y, br.w, br.h); cairo_stroke(cr);
                draw_text(cr, br.x + 14, br.y + 14,
                          b == 0 ? "Rename" : "Delete",
                          "Sans 16", 0.1, 0.1, 0.1);
            }
        }
        y += row_h_;
    }
}

BrowserHit FileBrowser::hit(double x, double y, const NotesIndex &idx) const {
    double bw = 210, bh = 80, m = 16;
    double btn_y = (header_h_ - bh) / 2.0;
    if (y >= btn_y && y <= btn_y + bh) {
        if (x >= w_ - m - bw && x <= w_ - m) return {BrowserAction::NewNote, -1};
        if (x >= w_ - m - 2*bw - 10 && x <= w_ - m - bw - 10)
            return {BrowserAction::NewMarkdown, -1};
    }
    if (y < header_h_) return {};

    int row = (int)((y - header_h_ - 6) / row_h_);
    if (row < 0) return {};
    double row_y = header_h_ + 6 + row * row_h_;

    // Adjust for the optional ".." parent row at the top of the list.
    int entry_idx = row;
    if (!current_path_.empty()) {
        if (row == 0) return {BrowserAction::OpenParent, -1};
        entry_idx = row - 1;
    }
    auto &entries = idx.entries();
    if (entry_idx < 0 || entry_idx >= (int)entries.size()) return {};

    if (!entries[entry_idx].is_folder) {
        Rect rn, dl;
        row_action_rects(w_, row_y, row_h_, rn, dl);
        if (rn.contains(x, y)) return {BrowserAction::Rename, entry_idx};
        if (dl.contains(x, y)) return {BrowserAction::Delete, entry_idx};
    }
    return {BrowserAction::Open, entry_idx};
}

} // namespace bn
