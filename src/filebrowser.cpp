#include "filebrowser.h"

#include "canvas.h"
#include "note_io.h"
#include "ui.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace bn {

using namespace theme;

namespace {

constexpr double kHeaderH  = 120.0;
constexpr double kTilePad  = 14.0;
constexpr double kTitleH   = 30.0;
constexpr double kSubH     = 22.0;
constexpr double kActionsH = 52.0;

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

// --- markdown preview cache ----------------------------------------------
// The first lines of each .md as Pango markup (headings bold), so a markdown
// tile shows its actual content. Same invalidation as the thumbnails.
struct PreviewEntry { std::string markup; uint32_t mtime = 0; bool tried = false; };
std::map<std::string, PreviewEntry> g_previews;

const std::string &get_md_preview(const IndexEntry &e) {
    PreviewEntry &slot = g_previews[e.id];
    if (slot.tried && slot.mtime == e.updated_ms) return slot.markup;
    slot.tried = true;
    slot.mtime = e.updated_ms;
    slot.markup.clear();

    std::string raw;
    if (!read_file(e.path, raw)) return slot.markup;
    if (raw.size() > 1500) {
        // Cut on a character boundary: Pango rejects invalid UTF-8 markup.
        size_t cut = 1500;
        while (cut > 0 && ((unsigned char)raw[cut] & 0xC0) == 0x80) --cut;
        raw.resize(cut);
    }
    size_t pos = 0;
    for (int lines = 0; pos < raw.size() && lines < 24; ++lines) {
        size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos
                                                                    : nl - pos);
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
        bool heading = !line.empty() && line[0] == '#';
        if (heading) line.erase(0, line.find_first_not_of("# "));
        gchar *esc = g_markup_escape_text(line.c_str(), -1);
        slot.markup += heading ? std::string("<b>") + esc + "</b>\n" : std::string(esc) + "\n";
        g_free(esc);
    }
    return slot.markup;
}

void draw_md_preview(cairo_t *cr, const Rect &r, const IndexEntry &e) {
    const std::string &markup = get_md_preview(e);
    cairo_save(cr);
    ui_round_rect_path(cr, r, kRadiusSm);
    cairo_clip(cr);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, (int)((r.w - 28) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_markup(l, markup.c_str(), -1);
    ui_grey(cr, 0.25);
    cairo_move_to(cr, r.x + 14, r.y + 14);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    cairo_restore(cr);
}

struct ActBtn {
    Rect r; BrowserAction action; const char *label; Icon icon; Btn style;
};

// Header buttons (right-aligned): New folder/note/markdown, settings and exit, or the
// move controls while a move is in flight.
void header_buttons(double w, bool move_mode, std::vector<ActBtn> &out) {
    out.clear();
    const double bw = 196, bh = 68, m = 20, g = 12;
    double by = (kHeaderH - bh) / 2.0;
    if (move_mode) {
        out.push_back({{w - m - bw, by, bw, bh}, BrowserAction::MoveHere,
                       "Move here", Icon::Check, Btn::Primary});
        out.push_back({{w - m - 2 * bw - g, by, bw, bh}, BrowserAction::MoveCancel,
                       "Cancel", Icon::Close, Btn::Secondary});
        return;
    }
    out.push_back({{w - m - bw, by, bw, bh}, BrowserAction::NewNote,
                   "Note", Icon::Plus, Btn::Primary});
    out.push_back({{w - m - 2 * bw - g, by, bw, bh}, BrowserAction::NewMarkdown,
                   "Markdown", Icon::Plus, Btn::Secondary});
    out.push_back({{w - m - 3 * bw - 2 * g, by, bw, bh}, BrowserAction::NewFolder,
                   "Folder", Icon::Plus, Btn::Secondary});
    // Icon-only settings and exit buttons to the left of the create buttons.
    out.push_back({{w - m - 3 * bw - 3 * g - bh, by, bh, bh}, BrowserAction::Settings,
                   "", Icon::Gear, Btn::Ghost});
    out.push_back({{w - m - 3 * bw - 4 * g - 2 * bh, by, bh, bh}, BrowserAction::Quit,
                   "", Icon::Power, Btn::Ghost});
}

// Per-tile actions along the bottom of an entry tile. Empty in move mode
// (tiles are tap-to-navigate then). Folders get Rename + Move; files get
// Rename + Move + Delete.
void tile_buttons(const Rect &tile, const IndexEntry &e, bool move_mode,
                  std::vector<ActBtn> &out) {
    out.clear();
    if (move_mode) return;
    const double bh = kActionsH - 8, m = 8, g = 4;
    struct Def { BrowserAction a; const char *label; Icon icon; };
    std::vector<Def> defs = {
        {BrowserAction::Rename, "Rename", Icon::Pencil},
        {BrowserAction::Move,   "Move",   Icon::Move},
    };
    if (!e.is_folder) defs.push_back({BrowserAction::Delete, "Delete", Icon::Trash});
    int n = (int)defs.size();
    double area_x = tile.x + m, area_w = tile.w - 2 * m;
    double by = tile.y + tile.h - bh - 4;
    double bw = (area_w - (n - 1) * g) / n;
    for (int i = 0; i < n; ++i)
        out.push_back({{area_x + i * (bw + g), by, bw, bh},
                       defs[i].a, defs[i].label, defs[i].icon, Btn::Ghost});
}

std::string kind_line(const IndexEntry &e) {
    std::string s = e.is_folder ? "Folder" : e.is_markdown ? "Markdown" : "Notebook";
    for (size_t i = 0; i < e.tags.size() && i < 3; ++i) s += "  #" + e.tags[i];
    return s;
}

} // namespace

FileBrowser::Grid FileBrowser::grid_metrics() const {
    Grid g;
    g.margin = 20.0;
    g.gap    = 20.0;
    g.top    = header_h_ + 20.0;
    const double min_tile = 340.0;
    g.cols = std::max(1, (int)std::floor((w_ - 2 * g.margin + g.gap) /
                                         (min_tile + g.gap)));
    g.tile_w = (w_ - 2 * g.margin - (g.cols - 1) * g.gap) / g.cols;
    // Portrait tile: a preview mirroring the page aspect, then the title, a
    // type/tags line and the action bar.
    double tw = g.tile_w - 2 * kTilePad;
    double aspect = (w_ > 0 && h_ > 0) ? (h_ / w_) : 1.3;
    double th = tw * aspect;
    g.tile_h = kTilePad + th + 10 + kTitleH + kSubH + 8 + kActionsH;
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
    ui_grey(cr, kCanvas);
    cairo_paint(cr);

    Grid g = grid_metrics();
    auto &entries = idx.entries();
    double tw     = g.tile_w - 2 * kTilePad;
    double aspect = (w_ > 0 && h_ > 0) ? (h_ / w_) : 1.3;
    double th     = tw * aspect;

    // Tiles (clipped to below the header so scrolled tiles don't overdraw it).
    cairo_save(cr);
    cairo_rectangle(cr, 0, header_h_, w_, h_ - header_h_);
    cairo_clip(cr);

    int cells = cell_count(idx);
    for (int cell = 0; cell < cells; ++cell) {
        Rect t = cell_rect(g, cell, scroll);
        if (t.y + t.h + kShadowDy < header_h_ || t.y > h_) continue;   // off-screen

        bool is_parent = has_parent() && cell == 0;
        const IndexEntry *e =
            is_parent ? nullptr : &entries[cell - (has_parent() ? 1 : 0)];

        ui_card(cr, t);

        // Preview area.
        Rect thumb{t.x + kTilePad, t.y + kTilePad, tw, th};
        double cx = thumb.x + thumb.w / 2.0, cy = thumb.y + thumb.h / 2.0;
        double icon_s = std::min(thumb.w, thumb.h) * 0.16;
        if (is_parent) {
            ui_fill_round(cr, thumb, kRadiusSm, kSurface);
            ui_grey(cr, kMuted);
            ui_icon(cr, Icon::Up, cx, cy, icon_s);
        } else if (e->is_folder) {
            ui_fill_round(cr, thumb, kRadiusSm, kSurface);
            ui_grey(cr, kMuted);
            ui_icon(cr, Icon::Folder, cx, cy, icon_s * 1.2);
        } else if (e->is_markdown) {
            ui_fill_round(cr, thumb, kRadiusSm, kPaper);
            draw_md_preview(cr, thumb, *e);
            ui_stroke_round(cr, thumb, kRadiusSm, kLine, 1.0);
        } else {
            ui_fill_round(cr, thumb, kRadiusSm, kPaper);
            if (cairo_surface_t *s = get_note_thumb(*e, w_, h_)) {
                int iw = cairo_image_surface_get_width(s);
                int ih = cairo_image_surface_get_height(s);
                cairo_save(cr);
                ui_round_rect_path(cr, thumb, kRadiusSm);
                cairo_clip(cr);
                cairo_translate(cr, thumb.x, thumb.y);
                cairo_scale(cr, thumb.w / (double)iw, thumb.h / (double)ih);
                cairo_set_source_surface(cr, s, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else {
                ui_grey(cr, kLine);
                ui_icon(cr, Icon::Notebook, cx, cy, icon_s);
            }
            ui_stroke_round(cr, thumb, kRadiusSm, kLine, 1.0);
        }

        // Title + type line.
        double ty = thumb.y + thumb.h + 10;
        ui_text_ellipsized(cr, thumb.x + 2, ty, thumb.w - 4,
                           is_parent ? "Up one level" : e->title,
                           "Sans Bold 18", kInk, false);
        ui_text_ellipsized(cr, thumb.x + 2, ty + kTitleH, thumb.w - 4,
                           is_parent ? ".." : kind_line(*e), kFontSmall, kMuted,
                           false);

        if (!is_parent) {
            std::vector<ActBtn> btns;
            tile_buttons(t, *e, move_mode, btns);
            if (!btns.empty())
                ui_hline(cr, t.x + 12, t.x + t.w - 12, btns[0].r.y - 4);
            for (auto &b : btns)
                ui_icon_button(cr, b.r, b.icon, b.label, b.style, "Sans 14");
        }

        // While moving, files can't be a destination: wash them out.
        if (move_mode && e && !e->is_folder) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
            ui_round_rect_path(cr, t, kRadius);
            cairo_fill(cr);
        }
        if (cell == focus_) ui_focus_ring(cr, t, kRadius);
    }

    // Empty folder.
    if (cells == 0) {
        double cy = header_h_ + (h_ - header_h_) * 0.32;
        ui_grey(cr, kLine);
        ui_icon(cr, Icon::Notebook, w_ / 2.0, cy, 48);
        ui_text_centered(cr, Rect{0, cy + 70, w_, 40}, "Nothing here yet",
                         "Sans Bold 24", kInk);
        ui_text_centered(cr, Rect{0, cy + 112, w_, 30},
                         "Tap Note to write by hand, or Markdown to type.",
                         kFontLabel, kMuted);
    }
    cairo_restore(cr);

    // Header (drawn last so tiles never overlap it).
    ui_grey(cr, kPaper);
    cairo_rectangle(cr, 0, 0, w_, header_h_); cairo_fill(cr);
    ui_hline(cr, 0, w_, header_h_ - 1, kLine, 1.5);

    ui_text(cr, 24, 20, "BetterNotes", "Sans Bold 30", kInk);
    if (move_mode) {
        ui_text_ellipsized(cr, 24, 72, w_ * 0.45,
                           "Moving \"" + move_title + "\": open a folder",
                           "Sans Bold 15", kInk, false);
    } else {
        std::string crumb = "Notes";
        if (!current_path_.empty()) {
            std::string p = current_path_;
            for (size_t i; (i = p.find('/')) != std::string::npos;) p.replace(i, 1, "  /  ");
            crumb += "  /  " + p;
        }
        ui_text_ellipsized(cr, 24, 72, w_ * 0.45, crumb, kFontLabel, kMuted, true);
    }

    std::vector<ActBtn> hb;
    header_buttons(w_, move_mode, hb);
    for (auto &b : hb)
        ui_icon_button(cr, b.r, b.icon, b.label, b.style, "Sans Bold 16");
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

int FileBrowser::entry_for_cell(int cell) const {
    if (has_parent()) --cell;
    return cell;   // -1 → the ".." tile
}

BrowserHit FileBrowser::activate(int cell, const NotesIndex &idx,
                                 bool move_mode) const {
    if (cell < 0 || cell >= cell_count(idx)) return {};
    int e = entry_for_cell(cell);
    if (e < 0) return {BrowserAction::OpenParent, -1};
    if (move_mode && !idx.entries()[e].is_folder) return {};
    return {BrowserAction::Open, e};
}

double FileBrowser::scroll_to_show(int cell, double scroll) const {
    Grid g = grid_metrics();
    Rect r = cell_rect(g, cell, 0.0);   // unscrolled position
    if (r.y - scroll < g.top)            scroll = r.y - g.top;
    else if (r.y + r.h - scroll > h_)    scroll = r.y + r.h - h_;
    return scroll < 0 ? 0 : scroll;
}

int FileBrowser::columns() const { return grid_metrics().cols; }

} // namespace bn
