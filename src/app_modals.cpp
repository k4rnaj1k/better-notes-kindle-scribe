// Modals: text input (rename / tags / folder / paths), delete confirmation,
// PDF export progress, and the markdown draw/OCR canvas.

#include "app.h"

#include "canvas.h"
#include "ui.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bn {

// ------------------------------------------------------------ geometry

Rect App::modal_card_rect() const {
    double cw = std::min(620.0, win_w_ * 0.84);
    double ch = 270.0;
    double cx = (win_w_ - cw) / 2.0;
    double cy = 170.0;   // near the top, leaving room for the keyboard below
    return Rect{cx, cy, cw, ch};
}

Rect App::modal_btn_rect(bool primary) const {
    Rect c = modal_card_rect();
    double bw = 160, bh = 60, gap = 14, m = 28;
    double by = c.y + c.h - bh - m;
    double px = primary ? (c.x + c.w - m - bw)             // right: Save/Delete
                        : (c.x + c.w - m - 2 * bw - gap);  // left:  Cancel
    return Rect{px, by, bw, bh};
}

Rect App::input_field_rect() const {
    Rect c = modal_card_rect();
    return Rect{c.x + 28, c.y + 82, c.w - 56, 64};
}

void App::redraw_input_card() { redraw_rect(modal_card_rect()); }

// ---------------------------------------------------------- input modal

void App::open_input(InputPurpose purpose, const std::string &title,
                     const std::string &initial, const std::string &target_id) {
    input_open_      = true;
    input_purpose_   = purpose;
    input_title_     = title;
    input_.set(initial);
    input_target_id_ = target_id;
    // A connected hardware keyboard makes the on-screen one redundant.
    keyboard_.set_visible(!hw_keyboard());
    redraw();
}

void App::close_input() {
    input_open_ = false;
    input_.clear();
    input_target_id_.clear();
    // Back to whatever the user had: the toolbar toggle owns the on-screen
    // keyboard outside the modal.
    keyboard_.set_visible(tool_.keyboard_visible && screen_ != Screen::Browser);
    redraw();
}

void App::commit_input() {
    std::string text = input_.text;
    std::string id   = input_target_id_;
    switch (input_purpose_) {
    case InputPurpose::Tags:
        commit_tags();
        close_input();          // stays on the note
        return;
    case InputPurpose::HomeFolder:
        close_input();          // settings modal stays open underneath
        set_home_folder(text);  // re-roots the vault + saves config
        redraw();
        return;
    case InputPurpose::ExportFolder:
        close_input();
        export_dir_ = text;     // "" is valid → export beside the note
        save_config();
        redraw();
        return;
    case InputPurpose::NewFolder:
        close_input();
        if (!text.empty()) index_.create_folder(text);
        enter_browser();        // rescans the dir + redraws
        return;
    case InputPurpose::Rename:
        close_input();
        if (!id.empty() && !text.empty()) index_.rename_entry(id, text);
        enter_browser();
        return;
    }
}

void App::draw_input_modal(cairo_t *cr) {
    using namespace theme;
    Rect c = modal_card_rect();
    ui_backdrop(cr, win_w_, win_h_);
    ui_card(cr, c);
    ui_text(cr, c.x + 28, c.y + 26, input_title_, kFontHead);

    // Tags: Page / Notebook scope as a segmented control.
    if (input_purpose_ == InputPurpose::Tags) {
        static const char *const kScopes[] = {"Page", "Notebook"};
        ui_segmented(cr, tags_scope_btn_rect(), kScopes, 2,
                     tags_scope_ == TagScope::Notebook ? 1 : 0);
    }

    // Field: the text scrolls left once the caret passes the right edge, so
    // long paths stay editable.
    Rect f = input_field_rect();
    ui_fill_round(cr, f, kRadiusSm, kPaper);
    ui_stroke_round(cr, f, kRadiusSm, kInk, 2.0);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 21");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, input_.text.c_str(), -1);
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    (void)tw;
    PangoRectangle pos;
    pango_layout_index_to_pos(l, (int)std::min(input_.cursor, input_.text.size()), &pos);
    double caret_x = pos.x / (double)PANGO_SCALE;
    double avail   = f.w - 36;
    double shift   = std::max(0.0, caret_x - avail);
    double tx = f.x + 16 - shift;
    double ty = f.y + (f.h - th) / 2.0;

    cairo_save(cr);
    cairo_rectangle(cr, f.x + 3, f.y + 3, f.w - 6, f.h - 6);
    cairo_clip(cr);
    ui_grey(cr, kInk);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, l);
    cairo_set_line_width(cr, 2.5);
    cairo_move_to(cr, tx + caret_x + 1, ty + 2);
    cairo_line_to(cr, tx + caret_x + 1, ty + th - 2);
    cairo_stroke(cr);
    cairo_restore(cr);
    g_object_unref(l);

    // Bottom row: layout badge + hint on the left, buttons on the right.
    Rect save = modal_btn_rect(true);
    double hx = c.x + 28;
    if (hw_keyboard() || keyboard_.visible()) {
        const char *badge = layout_badge(layout_);
        double bw = ui_pill_width(cr, badge, "Sans Bold 13");
        ui_pill(cr, Rect{hx, save.y + 16, bw, 28}, badge, false, "Sans Bold 13");
    }
    if (input_purpose_ == InputPurpose::Tags)
        ui_text(cr, f.x + 4, f.y + f.h + 8,
                hw_keyboard() ? "Comma-separated. Tab switches page / notebook."
                              : "Comma-separated.",
                kFontSmall, kMuted);
    ui_button(cr, save, "Save", Btn::Primary);
    ui_button(cr, modal_btn_rect(false), "Cancel", Btn::Secondary);
}

// ------------------------------------------------------- delete confirm

void App::open_delete_confirm(const IndexEntry &e) {
    confirm_open_      = true;
    confirm_target_id_ = e.id;
    confirm_text_      = "Delete \"" + e.title + "\"?";
    redraw();
}

void App::confirm_delete() {
    index_.remove_entry(confirm_target_id_);
    confirm_open_ = false;
    enter_browser();   // rescan + redraw
}

void App::cancel_confirm() {
    confirm_open_ = false;
    redraw();
}

void App::draw_confirm_modal(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ui_backdrop(cr, win_w, win_h);
    Rect c = modal_card_rect();
    ui_card(cr, c);
    Rect badge{c.x + 28, c.y + 30, 64, 64};
    ui_fill_round(cr, badge, 32, kSurface);
    ui_grey(cr, kInk);
    ui_icon(cr, Icon::Trash, badge.x + 32, badge.y + 32, 18);
    ui_text_wrapped(cr, c.x + 112, c.y + 32, c.w - 140, confirm_text_, kFontHead);
    ui_text(cr, c.x + 112, c.y + 104, "This can't be undone.", kFontSmall, kMuted);
    ui_button(cr, modal_btn_rect(true),  "Delete", Btn::Primary);
    ui_button(cr, modal_btn_rect(false), "Cancel", Btn::Secondary);
}

// ----------------------------------------------------------------- tags

std::vector<std::string> App::parse_tags(const std::string &csv) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        std::string t = csv.substr(i, j - i);
        size_t a = t.find_first_not_of(" \t\r\n#");
        size_t b = t.find_last_not_of(" \t\r\n");
        if (a != std::string::npos && b != std::string::npos && b >= a)
            out.push_back(t.substr(a, b - a + 1));
        if (j == csv.size()) break;
        i = j + 1;
    }
    return out;
}

std::string App::join_tags(const std::vector<std::string> &tags) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += ", ";
        out += tags[i];
    }
    return out;
}

void App::load_tags_into_input() {
    const std::vector<std::string> *src = &note_.tags;
    if (tags_scope_ == TagScope::Page &&
        current_page_ >= 0 && current_page_ < (int)note_.pages.size())
        src = &note_.pages[current_page_].tags;
    input_.set(join_tags(*src));
    input_title_ = (tags_scope_ == TagScope::Notebook)
                       ? "Tags — notebook" : "Tags — page";
}

void App::open_tags_editor() {
    // Tags live on native notes (notebooks + their pages); markdown files are
    // plain .md where tags are just #hashtags in the body.
    if (screen_ != Screen::NoteView) {
        status_ = "tags: open a notebook";
        redraw_toolbar();
        return;
    }
    tags_scope_ = TagScope::Page;
    open_input(InputPurpose::Tags, "", "");
    load_tags_into_input();
}

void App::toggle_tags_scope() {
    commit_tags();   // keep the current scope's edits
    tags_scope_ = (tags_scope_ == TagScope::Page) ? TagScope::Notebook
                                                  : TagScope::Page;
    load_tags_into_input();
    redraw_input_card();
}

void App::commit_tags() {
    std::vector<std::string> parsed = parse_tags(input_.text);
    if (tags_scope_ == TagScope::Notebook) {
        note_.tags = std::move(parsed);
    } else if (current_page_ >= 0 && current_page_ < (int)note_.pages.size()) {
        note_.pages[current_page_].tags = std::move(parsed);
    }
    note_.mark_dirty();
}

Rect App::tags_scope_btn_rect() const {
    Rect c = modal_card_rect();
    double bw = 300, bh = 50;
    return Rect{c.x + c.w - 28 - bw, c.y + 18, bw, bh};
}

// ---------------------------------------------------------- PDF export

Rect App::export_overlay_rect() const {
    double cw = std::min(500.0, win_w_ * 0.8);
    double ch = 170.0;
    return Rect{(win_w_ - cw) / 2.0, (win_h_ - ch) / 2.0, cw, ch};
}

void App::draw_export_overlay(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ui_backdrop(cr, win_w, win_h);
    Rect c = export_overlay_rect();
    ui_card(cr, c);
    ui_text(cr, c.x + 28, c.y + 24, "Exporting PDF", kFontHead);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "page %d of %d",
                  std::min(export_done_, export_total_), export_total_);
    ui_text(cr, c.x + 28, c.y + 62, buf, kFontSmall, kMuted);

    // Rounded progress bar.
    Rect track{c.x + 28, c.y + c.h - 50, c.w - 56, 20};
    double frac = export_total_ > 0
        ? (double)export_done_ / (double)export_total_ : 0.0;
    frac = std::clamp(frac, 0.0, 1.0);
    ui_fill_round(cr, track, 10, kSurface);
    if (frac > 0)
        ui_fill_round(cr, Rect{track.x, track.y, std::max(20.0, track.w * frac),
                               track.h}, 10, kInk);
}

// ---------------------------------------------- draw / OCR modal (markdown)

Rect App::draw_canvas_rect() const {
    const double bar = 88.0;   // top button bar
    return Rect{0, bar, (double)win_w_, (double)win_h_ - bar};
}

Rect App::draw_btn_rect(int which) const {
    const double bw = 150, bh = 60, gap = 12, m = 16, y = 14;
    if (which == 2)   // Cancel hugs the right edge
        return Rect{(double)win_w_ - m - bw, y, bw, bh};
    return Rect{m + which * (bw + gap), y, bw, bh};  // 0=Save, 1=Clear
}

void App::open_draw_modal(DrawPurpose purpose) {
    draw_open_    = true;
    draw_purpose_ = purpose;
    clear_draw_modal();
    status_ = (purpose == DrawPurpose::Ocr) ? "draw a word, then Save"
                                            : "draw, then Save";
}

void App::clear_draw_modal() {
    draw_strokes_.clear();
    draw_live_         = Stroke{};
    draw_pen_down_     = false;
    draw_erase_active_ = false;
    draw_ink_bbox_     = {0, 0, 0, 0};
    ocr_preview_.clear();
    redraw();
}

void App::close_draw_modal() {
    draw_open_ = false;
    clear_draw_modal();
}

bool App::draw_render_png(const std::string &abs_path) {
    if (draw_strokes_.empty()) return false;
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (auto &s : draw_strokes_) {
        Rect b = s.bbox();
        minx = std::min(minx, b.x);       miny = std::min(miny, b.y);
        maxx = std::max(maxx, b.x + b.w); maxy = std::max(maxy, b.y + b.h);
    }
    const double pad = 16.0;
    int w = (int)std::ceil(maxx - minx + 2 * pad);
    int h = (int)std::ceil(maxy - miny + 2 * pad);
    if (w < 1 || h < 1) return false;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_translate(cr, pad - minx, pad - miny);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    for (auto &s : draw_strokes_) canvas_render_stroke(cr, s);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, abs_path.c_str());
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS;
}

void App::commit_draw_modal() {
    if (draw_strokes_.empty()) { close_draw_modal(); return; }

    if (draw_purpose_ == DrawPurpose::Ocr) {
        Rect canvas = draw_canvas_rect();
        // Prefer the candidate already shown in the bar (what the user saw);
        // fall back to a fresh recognise if no preview was computed yet.
        std::string word = !ocr_preview_.empty()
            ? ocr_preview_
            : ocr_.recognize(draw_strokes_, (int)canvas.w, (int)canvas.h);
        if (!word.empty()) {
            markdown_snapshot();
            markdown_insert(word);
            status_ = "ocr → " + word;
        } else {
            status_ = "ocr: nothing recognised";
        }
        close_draw_modal();
        return;
    }

    // Image: rasterise to attachments/drawing-N.png next to the .md, then
    // insert a standard image link at the cursor.
    std::string md_dir = markdown_path_;
    size_t slash = md_dir.find_last_of('/');
    md_dir = (slash == std::string::npos) ? "." : md_dir.substr(0, slash);
    std::string att = md_dir + "/attachments";
    ensure_dir(att);

    std::string fname;
    for (int i = 1; i < 100000; ++i) {
        char b[32]; std::snprintf(b, sizeof(b), "drawing-%d.png", i);
        if (!path_exists(att + "/" + b)) { fname = b; break; }
    }
    if (!fname.empty() && draw_render_png(att + "/" + fname)) {
        markdown_snapshot();
        markdown_insert("![](attachments/" + fname + ")");
        status_ = "inserted " + fname;
    } else {
        status_ = "drawing save failed";
    }
    close_draw_modal();
}

void App::draw_draw_modal(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    Rect canvas = draw_canvas_rect();

    // White drawing surface with a faint frame.
    ui_grey(cr, kPaper);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);
    ui_stroke_round(cr, Rect{canvas.x + 12, canvas.y + 12, canvas.w - 24,
                             canvas.h - 24}, kRadius, kLine, 1.0);

    // Committed + live ink, hard-edged to match the inkfb fast path.
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    for (auto &s : draw_strokes_) canvas_render_stroke(cr, s);
    if (draw_pen_down_) canvas_render_stroke(cr, draw_live_);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    // Top bar.
    ui_grey(cr, kPaper);
    cairo_rectangle(cr, 0, 0, win_w, canvas.y);
    cairo_fill(cr);
    ui_hline(cr, 0, win_w, canvas.y - 1, kLine, 1.5);

    ui_button(cr, draw_btn_rect(0), "Save",   Btn::Primary);
    ui_button(cr, draw_btn_rect(1), "Clear",  Btn::Secondary);
    ui_button(cr, draw_btn_rect(2), "Cancel", Btn::Secondary);

    Rect clear = draw_btn_rect(1);
    double hx = clear.x + clear.w + 28;
    double hw = draw_btn_rect(2).x - hx - 20;
    if (draw_purpose_ == DrawPurpose::Ocr) {
        // Show the live recognition so the user sees what Save will insert.
        ui_text(cr, hx, 16, "Handwriting to text", kFontSmall, kMuted);
        if (ocr_preview_.empty())
            ui_text_ellipsized(cr, hx, 40, hw, "write a word below", kFontLabel,
                               kMuted, false);
        else
            ui_text_ellipsized(cr, hx, 38, hw, ocr_preview_, "Sans Bold 20",
                               kInk, false);
    } else {
        ui_text(cr, hx, 16, "Drawing", kFontSmall, kMuted);
        ui_text_ellipsized(cr, hx, 40, hw, "saved as an image in the note",
                           kFontLabel, kMuted, false);
    }
}

} // namespace bn
