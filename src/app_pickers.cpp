// List pickers: link target (after a lasso) and page template. Both share the
// ListModal geometry from ui.h, so draw and hit-test always agree.

#include "app.h"

#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace bn {

namespace {

constexpr double kLinkPickerW = 640, kLinkPickerH = 960;
constexpr double kTmplPickerW = 560, kTmplPickerH = 880;

// Shared press handling for a scrollable list modal: close box / outside →
// Close, top/bottom 40 px of the list → page the scroll (Scrolled), a row →
// Row with its absolute index in row_idx.
enum class ListPress { Close, Scrolled, Row, Nothing };

ListPress list_press(const ListModal &m, double x, double y, int &scroll,
                     int total, int &row_idx) {
    if (m.close.contains(x, y) || m.outside(x, y)) return ListPress::Close;
    if (y >= m.list_top && y < m.list_top + 40 && scroll > 0) {
        scroll = std::max(0, scroll - m.rows_visible);
        return ListPress::Scrolled;
    }
    if (y >= m.list_bot - 40 && y <= m.list_bot &&
        scroll + m.rows_visible < total) {
        scroll = std::min(total - 1, scroll + m.rows_visible);
        return ListPress::Scrolled;
    }
    int r = m.row_at(y);
    if (r < 0 || scroll + r >= total) return ListPress::Nothing;
    row_idx = scroll + r;
    return ListPress::Row;
}

// Row chrome shared by both pickers: a filled row for the keyboard selection,
// otherwise a hairline above every row but the first.
void draw_row_bg(cairo_t *cr, const ListModal &m, int visible_row, bool selected) {
    Rect r = m.row_rect(visible_row);
    if (selected) ui_fill_round(cr, r, theme::kRadiusSm, theme::kSurface);
    else if (visible_row > 0)
        ui_hline(cr, r.x + 8, r.x + r.w - 8, r.y - 3);
}

// Miniature of a built-in paper template inside r.
void draw_template_swatch(cairo_t *cr, const Rect &r, int which) {
    ui_fill_round(cr, r, 6, theme::kPaper);
    cairo_save(cr);
    ui_round_rect_path(cr, r, 6);
    cairo_clip(cr);
    ui_grey(cr, theme::kBorder);
    cairo_set_line_width(cr, 1.0);
    const double step = 9;
    if (which == 1) {                                  // ruled
        for (double y = r.y + step; y < r.y + r.h; y += step) {
            cairo_move_to(cr, r.x, y + 0.5); cairo_line_to(cr, r.x + r.w, y + 0.5);
        }
        cairo_stroke(cr);
    } else if (which == 2) {                           // grid
        for (double y = r.y + step; y < r.y + r.h; y += step) {
            cairo_move_to(cr, r.x, y + 0.5); cairo_line_to(cr, r.x + r.w, y + 0.5);
        }
        for (double x = r.x + step; x < r.x + r.w; x += step) {
            cairo_move_to(cr, x + 0.5, r.y); cairo_line_to(cr, x + 0.5, r.y + r.h);
        }
        cairo_stroke(cr);
    } else if (which == 3) {                           // dots
        for (double y = r.y + step; y < r.y + r.h; y += step)
            for (double x = r.x + step; x < r.x + r.w; x += step) {
                cairo_arc(cr, x, y, 1.1, 0, 2 * M_PI);
                cairo_fill(cr);
            }
    }
    cairo_restore(cr);
    ui_stroke_round(cr, r, 6, theme::kBorder, 1.5);
}

} // namespace

// ---------------------------------------------------------------- keys

bool App::key_list_modal(const KeyEvent &e) {
    bool link   = picker_.open;
    int &sel    = link ? picker_.sel    : tmpl_sel_;
    int &scroll = link ? picker_.scroll : tmpl_scroll_;
    int total   = link ? (int)picker_.entries.size() : template_count();
    ListModal m = link ? ListModal::make(win_w_, win_h_, kLinkPickerW, kLinkPickerH)
                       : ListModal::make(win_w_, win_h_, kTmplPickerW, kTmplPickerH);
    int page = std::max(1, m.rows_visible);

    int nsel = sel;
    switch (e.key) {
    case Key::Escape:
        if (link) picker_.open = false; else tmpl_open_ = false;
        redraw();
        return true;
    case Key::Enter:
        if (link) pick_link(sel); else pick_template(sel);
        return true;
    case Key::Up:       nsel = sel - 1;    break;
    case Key::Down:     nsel = sel + 1;    break;
    case Key::PageUp:   nsel = sel - page; break;
    case Key::PageDown: nsel = sel + page; break;
    case Key::Home:     nsel = 0;          break;
    case Key::End:      nsel = total - 1;  break;
    default: return false;
    }
    if (total <= 0) return true;
    sel = std::clamp(nsel, 0, total - 1);
    // Keep the selection on screen.
    if (sel < scroll)             scroll = sel;
    else if (sel >= scroll + page) scroll = sel - page + 1;
    redraw_rect(m.card);
    return true;
}

// ------------------------------------------------------------ link picker

void App::open_link_picker(const Rect &anchor) {
    picker_.open    = true;
    picker_.anchor  = anchor;
    picker_.page    = current_page_;
    picker_.scroll  = 0;
    picker_.sel     = 0;
    picker_.entries = index_.walk_vault();
}

void App::handle_picker_press(double x, double y) {
    ListModal m = ListModal::make(win_w_, win_h_, kLinkPickerW, kLinkPickerH);
    int idx = -1;
    switch (list_press(m, x, y, picker_.scroll, (int)picker_.entries.size(), idx)) {
    case ListPress::Close:    picker_.open = false; redraw(); break;
    case ListPress::Scrolled: redraw(); break;
    case ListPress::Row:      pick_link(idx); break;
    case ListPress::Nothing:  break;
    }
}

void App::pick_link(int idx) {
    if (idx < 0 || idx >= (int)picker_.entries.size()) return;
    const auto &e = picker_.entries[idx];
    Link l;
    l.page = picker_.page;
    l.rect = picker_.anchor;
    // Store as wiki-link name (basename without .md). resolve_link does fuzzy
    // matching at follow time.
    std::string name = e.id;
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0)
        name.resize(name.size() - 3);
    l.target = name;
    note_.links.push_back(l);
    note_.mark_dirty();
    status_ = "linked → " + e.title;
    picker_.open = false;
    redraw();
}

void App::draw_link_picker(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ListModal m = ListModal::make(win_w, win_h, kLinkPickerW, kLinkPickerH);
    m.draw_frame(cr, win_w, win_h, "Link to", "pick the note the circled area opens");

    const auto &es = picker_.entries;
    for (int i = 0; i < m.rows_visible; ++i) {
        int idx = picker_.scroll + i;
        if (idx >= (int)es.size()) break;
        bool sel = hw_keyboard() && idx == picker_.sel;
        draw_row_bg(cr, m, i, sel);
        Rect r = m.row_rect(i);

        ui_grey(cr, kMuted);
        ui_icon(cr, es[idx].is_markdown ? Icon::Markdown : Icon::Notebook,
                r.x + 32, r.y + r.h / 2.0, 13);
        // Title, then the folder it lives in.
        std::string folder = es[idx].id;
        auto slash = folder.find_last_of('/');
        folder = (slash == std::string::npos) ? "" : folder.substr(0, slash);
        double tx = r.x + 64, tw = r.w - 80;
        if (folder.empty()) {
            ui_text_ellipsized(cr, tx, r.y + (r.h - 26) / 2.0, tw, es[idx].title,
                               kFontBody, kInk, false);
        } else {
            ui_text_ellipsized(cr, tx, r.y + 8, tw, es[idx].title, kFontBody, kInk, false);
            ui_text_ellipsized(cr, tx, r.y + 38, tw, folder, kFontSmall, kMuted, true);
        }
    }

    // Scroll hints
    if (picker_.scroll > 0)
        ui_text(cr, m.card.x + m.card.w - 260, m.card.y + 66,
                "tap top edge: scroll up", kFontSmall, kMuted);
    if (picker_.scroll + m.rows_visible < (int)es.size())
        ui_text_centered(cr, Rect{m.card.x, m.list_bot - 4, m.card.w, 22},
                         "tap bottom edge for more", kFontSmall, kMuted);
}

// -------------------------------------------------------- template picker

void App::open_template_picker() {
    if (screen_ != Screen::NoteView) return;
    tmpl_custom_.clear();
    for (auto &f : list_dir(notes_dir_ + "/templates")) {
        if (f.size() > 4) {
            std::string ext = f.substr(f.size() - 4);
            for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".png") tmpl_custom_.push_back(f);
        }
    }
    std::sort(tmpl_custom_.begin(), tmpl_custom_.end());
    tmpl_open_   = true;
    tmpl_scroll_ = 0;
    tmpl_sel_    = 0;
    redraw();
}

void App::apply_template(TemplateId tmpl, const std::string &bg_image) {
    if (current_page_ >= 0 && current_page_ < (int)note_.pages.size()) {
        note_.pages[current_page_].tmpl     = tmpl;
        note_.pages[current_page_].bg_image = bg_image;
        // The background changed under the strokes — drop this page's cached
        // bitmap so it re-renders with the new template.
        invalidate_page_cache(current_page_);
    }
    // New pages inherit the most-recently chosen template.
    note_.default_template = tmpl;
    note_.default_bg_image = bg_image;
    note_.mark_dirty();
}

void App::pick_template(int idx) {
    if (idx < 0 || idx >= template_count()) return;
    static const TemplateId kBuiltin[] = {
        TemplateId::Blank, TemplateId::Ruled, TemplateId::Grid, TemplateId::Dot,
    };
    if (idx < 4) apply_template(kBuiltin[idx], "");
    else         apply_template(TemplateId::Blank, "templates/" + tmpl_custom_[idx - 4]);
    status_ = "template applied";
    tmpl_open_ = false;
    redraw();
}

void App::handle_template_picker_press(double x, double y) {
    ListModal m = ListModal::make(win_w_, win_h_, kTmplPickerW, kTmplPickerH);
    int idx = -1;
    switch (list_press(m, x, y, tmpl_scroll_, template_count(), idx)) {
    case ListPress::Close:    tmpl_open_ = false; redraw(); break;
    case ListPress::Scrolled: redraw(); break;
    case ListPress::Row:      pick_template(idx); break;
    case ListPress::Nothing:  break;
    }
}

void App::draw_template_picker(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ListModal m = ListModal::make(win_w, win_h, kTmplPickerW, kTmplPickerH);
    m.draw_frame(cr, win_w, win_h, "Page template",
                 tmpl_custom_.empty() ? "add your own: PNGs in the vault's templates/ folder"
                                      : "built-in papers and your PNGs");

    std::vector<std::string> labels = {"Blank", "Ruled", "Grid", "Dots"};
    for (auto &f : tmpl_custom_) labels.push_back(f);
    const Page *cur = (current_page_ >= 0 && current_page_ < (int)note_.pages.size())
                          ? &note_.pages[current_page_] : nullptr;

    for (int i = 0; i < m.rows_visible; ++i) {
        int idx = tmpl_scroll_ + i;
        if (idx >= (int)labels.size()) break;
        bool sel = hw_keyboard() && idx == tmpl_sel_;
        draw_row_bg(cr, m, i, sel);
        Rect r = m.row_rect(i);

        Rect sw{r.x + 14, r.y + 8, 44, r.h - 16};
        if (idx < 4) {
            draw_template_swatch(cr, sw, idx);
        } else {
            ui_fill_round(cr, sw, 6, kSurface);
            ui_text_centered(cr, sw, "PNG", "Sans Bold 10", kMuted);
        }
        ui_text_ellipsized(cr, r.x + 76, r.y + (r.h - 26) / 2.0, r.w - 140,
                           labels[idx], kFontBody, kInk, false);

        bool active = false;
        if (cur) {
            if (idx < 4) active = cur->bg_image.empty() && (int)cur->tmpl == idx;
            else         active = cur->bg_image == ("templates/" + tmpl_custom_[idx - 4]);
        }
        if (active) {
            ui_grey(cr, kInk);
            ui_icon(cr, Icon::Check, r.x + r.w - 32, r.y + r.h / 2.0, 13);
        }
    }

    if (tmpl_scroll_ + m.rows_visible < (int)labels.size())
        ui_text_centered(cr, Rect{m.card.x, m.list_bot - 4, m.card.w, 22},
                         "tap bottom edge for more", kFontSmall, kMuted);
}

} // namespace bn
