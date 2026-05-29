#include "filebrowser.h"

#include "canvas.h"
#include "note_io.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace bn {

namespace {

constexpr double kHeaderH = 120.0;
constexpr double kTilePad = 10.0;

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

// Centred, single-line, ellipsized title under a thumbnail.
void draw_title(cairo_t *cr, double x, double y, double width, const char *t) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 18");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_set_text(l, t, -1);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

void draw_btn(cairo_t *cr, const Rect &r, const char *label, bool primary) {
    cairo_set_source_rgb(cr, primary ? 0.20 : 0.90,
                             primary ? 0.20 : 0.90,
                             primary ? 0.20 : 0.90);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_stroke(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 15");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, label, -1);
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    double fg = primary ? 0.95 : 0.12;
    cairo_set_source_rgb(cr, fg, fg, fg);
    cairo_move_to(cr, r.x + (r.w - tw) / 2.0, r.y + (r.h - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

// --- placeholder glyphs for non-notebook tiles ---------------------------
void icon_stroke(cairo_t *cr, const Rect &r, double frac) {
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, std::max(2.0, std::min(r.w, r.h) * frac));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
}

void draw_folder_icon(cairo_t *cr, const Rect &r) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double s = std::min(r.w, r.h) * 0.26;
    icon_stroke(cr, r, 0.012);
    cairo_move_to(cr, cx - s,       cy - s * 0.55);
    cairo_line_to(cr, cx - s * 0.2, cy - s * 0.55);
    cairo_line_to(cr, cx,           cy - s * 0.2);
    cairo_line_to(cr, cx + s,       cy - s * 0.2);
    cairo_line_to(cr, cx + s,       cy + s * 0.75);
    cairo_line_to(cr, cx - s,       cy + s * 0.75);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

void draw_page_icon(cairo_t *cr, const Rect &r, bool lines) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double w = std::min(r.w, r.h) * 0.34, h = w * 1.3;
    icon_stroke(cr, r, 0.010);
    cairo_rectangle(cr, cx - w / 2.0, cy - h / 2.0, w, h); cairo_stroke(cr);
    if (lines)
        for (int i = 0; i < 4; ++i) {
            double yy = cy - h / 2.0 + h * 0.28 + i * (h * 0.16);
            cairo_move_to(cr, cx - w * 0.32, yy);
            cairo_line_to(cr, cx + w * 0.32, yy);
            cairo_stroke(cr);
        }
}

void draw_parent_icon(cairo_t *cr, const Rect &r) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double s = std::min(r.w, r.h) * 0.22;
    icon_stroke(cr, r, 0.014);
    cairo_move_to(cr, cx - s, cy + s * 0.3);
    cairo_line_to(cr, cx,     cy - s * 0.6);
    cairo_line_to(cr, cx + s, cy + s * 0.3);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - s * 0.6);
    cairo_line_to(cr, cx, cy + s * 0.7);
    cairo_stroke(cr);
}

// --- first-page thumbnail cache ------------------------------------------
// Rendered first pages, keyed by note id. Held for the process lifetime and
// invalidated when the note's mtime changes, so the grid renders each page
// once and reuses it on every later repaint/scroll. nullptr is cached too (a
// failed/empty load isn't retried).
struct ThumbEntry { cairo_surface_t *surf = nullptr; uint32_t mtime = 0; bool tried = false; };
std::map<std::string, ThumbEntry> g_thumbs;

cairo_surface_t *get_note_thumb(const IndexEntry &e,
                                double page_w, double page_h) {
    ThumbEntry &slot = g_thumbs[e.id];
    if (slot.tried && slot.mtime == e.updated_ms) return slot.surf;
    if (slot.surf) { cairo_surface_destroy(slot.surf); slot.surf = nullptr; }
    slot.tried = true;
    slot.mtime = e.updated_ms;
    if (page_w <= 0 || page_h <= 0) return nullptr;

    Note n;
    if (!load_note(e.path, n) || n.pages.empty()) return nullptr;

    const double TW = 240.0;                 // modest render width; upscaled to fit
    double s  = TW / page_w;
    int    tw = (int)TW;
    int    th = std::max(1, (int)std::lround(page_h * s));
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
    cairo_t *c = cairo_create(surf);
    cairo_set_source_rgb(c, 1, 1, 1); cairo_paint(c);
    cairo_scale(c, s, s);                     // strokes are in full-page coords
    canvas_render_page(c, n.pages[0], page_w, page_h);
    cairo_destroy(c);
    slot.surf = surf;
    return surf;
}

struct ActBtn { Rect r; BrowserAction action; const char *label; };

// Header buttons (right-aligned): New folder/note/markdown, or the move
// controls while a move is in flight.
void header_buttons(double w, bool move_mode, std::vector<ActBtn> &out) {
    out.clear();
    const double bw = 200, bh = 76, m = 16, g = 10;
    double by = (kHeaderH - bh) / 2.0;
    if (move_mode) {
        out.push_back({{w - m - bw,            by, bw, bh},
                       BrowserAction::MoveHere,   "Move here"});
        out.push_back({{w - m - 2 * bw - g,    by, bw, bh},
                       BrowserAction::MoveCancel, "Cancel"});
    } else {
        out.push_back({{w - m - bw,            by, bw, bh},
                       BrowserAction::NewNote,     "+ Note"});
        out.push_back({{w - m - 2 * bw - g,    by, bw, bh},
                       BrowserAction::NewMarkdown, "+ Markdown"});
        out.push_back({{w - m - 3 * bw - 2 * g, by, bw, bh},
                       BrowserAction::NewFolder,   "+ Folder"});
    }
}

// Per-tile action buttons along the bottom of an entry tile. Empty in move
// mode (tiles are tap-to-navigate then). Folders get Rename + Move; files get
// Rename + Move + Delete.
void tile_buttons(const Rect &tile, const IndexEntry &e, bool move_mode,
                  std::vector<ActBtn> &out) {
    out.clear();
    if (move_mode) return;
    const double bh = 42, m = 8, g = 6;
    std::vector<std::pair<BrowserAction, const char *>> defs;
    defs.push_back({BrowserAction::Rename, "Rename"});
    defs.push_back({BrowserAction::Move,   "Move"});
    if (!e.is_folder) defs.push_back({BrowserAction::Delete, "Delete"});
    int n = (int)defs.size();
    double area_x = tile.x + m, area_w = tile.w - 2 * m;
    double by = tile.y + tile.h - bh - m;
    double bw = (area_w - (n - 1) * g) / n;
    for (int i = 0; i < n; ++i)
        out.push_back({{area_x + i * (bw + g), by, bw, bh},
                       defs[i].first, defs[i].second});
}

} // namespace

FileBrowser::Grid FileBrowser::grid_metrics() const {
    Grid g;
    g.margin = 16.0;
    g.gap    = 16.0;
    g.top    = header_h_ + 8.0;
    const double min_tile = 340.0;
    g.cols = std::max(1, (int)std::floor((w_ - 2 * g.margin + g.gap) /
                                         (min_tile + g.gap)));
    g.tile_w = (w_ - 2 * g.margin - (g.cols - 1) * g.gap) / g.cols;
    // Portrait tile: a thumbnail mirroring the page aspect, then a title row
    // and the action-button row.
    double tw = g.tile_w - 2 * kTilePad;
    double aspect = (w_ > 0 && h_ > 0) ? (h_ / w_) : 1.3;
    double th = tw * aspect;
    g.tile_h = kTilePad + th + 8 + 28 + 6 + 50 + kTilePad;
    return g;
}

Rect FileBrowser::cell_rect(const Grid &g, int cell, double scroll) const {
    int row = cell / g.cols;
    int col = cell % g.cols;
    double x = g.margin + col * (g.tile_w + g.gap);
    double y = g.top + row * (g.tile_h + g.gap) - scroll;
    return Rect{x, y, g.tile_w, g.tile_h};
}

int FileBrowser::cell_count(const NotesIndex &idx) const {
    return (int)idx.entries().size() + (has_parent() ? 1 : 0);
}

double FileBrowser::content_height(const NotesIndex &idx) const {
    Grid g = grid_metrics();
    int cells = cell_count(idx);
    int rows = (cells + g.cols - 1) / g.cols;
    return g.top + rows * (g.tile_h + g.gap) + 8.0;
}

void FileBrowser::layout(double w, double h, size_t) {
    w_ = w; h_ = h;
}

void FileBrowser::draw(cairo_t *cr, const NotesIndex &idx, double w, double h,
                       double scroll, bool move_mode,
                       const std::string &move_title) {
    w_ = w; h_ = h;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    Grid g = grid_metrics();
    auto &entries = idx.entries();
    double tw     = g.tile_w - 2 * kTilePad;
    double aspect = (w_ > 0 && h_ > 0) ? (h_ / w_) : 1.3;
    double th     = tw * aspect;

    // Tiles (clipped to below the header so scrolled tiles don't overdraw it).
    cairo_save(cr);
    cairo_rectangle(cr, 0, g.top, w_, h_ - g.top);
    cairo_clip(cr);

    int cells = cell_count(idx);
    for (int cell = 0; cell < cells; ++cell) {
        Rect t = cell_rect(g, cell, scroll);
        if (t.y + t.h < g.top || t.y > h_) continue;   // off-screen

        bool is_parent = has_parent() && cell == 0;
        const IndexEntry *e =
            is_parent ? nullptr : &entries[cell - (has_parent() ? 1 : 0)];

        // Card background + border.
        cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
        cairo_rectangle(cr, t.x, t.y, t.w, t.h); cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, t.x, t.y, t.w, t.h); cairo_stroke(cr);

        Rect thumb{t.x + kTilePad, t.y + kTilePad, tw, th};
        double title_y = thumb.y + thumb.h + 6;

        // Thumbnail area: white page, then content or a placeholder glyph.
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, thumb.x, thumb.y, thumb.w, thumb.h); cairo_fill(cr);

        if (is_parent) {
            draw_parent_icon(cr, thumb);
        } else if (e->is_folder) {
            draw_folder_icon(cr, thumb);
        } else if (e->is_markdown) {
            draw_page_icon(cr, thumb, true);
        } else {
            cairo_surface_t *s = get_note_thumb(*e, w_, h_);
            if (s) {
                int iw = cairo_image_surface_get_width(s);
                int ih = cairo_image_surface_get_height(s);
                cairo_save(cr);
                cairo_translate(cr, thumb.x, thumb.y);
                cairo_scale(cr, thumb.w / (double)iw, thumb.h / (double)ih);
                cairo_set_source_surface(cr, s, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else {
                draw_page_icon(cr, thumb, false);
            }
        }
        // Thumbnail frame, dimmed for files while moving (can't drop onto one).
        bool dim = move_mode && e && !e->is_folder;
        cairo_set_source_rgb(cr, dim ? 0.7 : 0.5,
                                 dim ? 0.7 : 0.5,
                                 dim ? 0.7 : 0.5);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, thumb.x, thumb.y, thumb.w, thumb.h); cairo_stroke(cr);

        // Title.
        const char *title = is_parent ? ".." : e->title.c_str();
        draw_title(cr, thumb.x, title_y, thumb.w, title);

        if (is_parent) continue;   // no action buttons on the parent tile

        std::vector<ActBtn> btns;
        tile_buttons(t, *e, move_mode, btns);
        for (auto &b : btns) draw_btn(cr, b.r, b.label, false);
    }
    cairo_restore(cr);

    // Header (drawn last so tiles never overlap it).
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, 0, 0, w_, header_h_); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, header_h_); cairo_line_to(cr, w_, header_h_);
    cairo_stroke(cr);

    draw_text(cr, 20, 18, "BetterNotes", "Sans Bold 30", 0.1, 0.1, 0.1);
    std::string crumb = current_path_.empty() ? "/" : "/" + current_path_;
    draw_text(cr, 20, 64, crumb.c_str(), "Sans 16", 0.35, 0.35, 0.35);
    if (move_mode) {
        std::string m = "Moving \"" + move_title +
                        "\" — open a folder, then tap Move here";
        draw_text(cr, 20, 90, m.c_str(), "Sans 15", 0.15, 0.15, 0.15);
    }

    std::vector<ActBtn> hb;
    header_buttons(w_, move_mode, hb);
    for (auto &b : hb)
        draw_btn(cr, b.r, b.label, b.action == BrowserAction::MoveHere);
}

BrowserHit FileBrowser::hit(double x, double y, const NotesIndex &idx,
                            double scroll, bool move_mode) const {
    // Header buttons take priority.
    std::vector<ActBtn> hb;
    header_buttons(w_, move_mode, hb);
    for (auto &b : hb)
        if (b.r.contains(x, y)) return {b.action, -1};
    if (y < header_h_) return {};

    Grid g = grid_metrics();
    auto &entries = idx.entries();
    int cells = cell_count(idx);
    for (int cell = 0; cell < cells; ++cell) {
        Rect t = cell_rect(g, cell, scroll);
        if (!t.contains(x, y)) continue;

        bool is_parent = has_parent() && cell == 0;
        if (is_parent) return {BrowserAction::OpenParent, -1};

        int entry_idx = cell - (has_parent() ? 1 : 0);
        if (entry_idx < 0 || entry_idx >= (int)entries.size()) return {};
        const IndexEntry &e = entries[entry_idx];

        // Tile action buttons (none in move mode).
        std::vector<ActBtn> tb;
        tile_buttons(t, e, move_mode, tb);
        for (auto &b : tb)
            if (b.r.contains(x, y)) return {b.action, entry_idx};

        // Body tap: open folders (and navigate during a move); files only open
        // when not moving (you can't drop onto a file).
        if (move_mode && !e.is_folder) return {};
        return {BrowserAction::Open, entry_idx};
    }
    return {};
}

} // namespace bn
