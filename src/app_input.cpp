// Pointer (finger) input and toolbar / browser actions.
//
// The stylus draws through the evdev pen reader (app_pen.cpp); GTK pointer
// events here are finger taps and swipes. Most things act on release, so a
// press only records the gesture start and gives visual press feedback.

#include "app.h"

#include "pages.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>

namespace bn {

namespace {

constexpr double kDragMin  = 24.0;    // px before a tap becomes a scroll drag
constexpr double kSwipeMin = 120.0;   // px for a horizontal page-flip swipe

} // namespace

// ------------------------------------------------------------------- press

bool App::on_button_press(double x, double y) {
    // PDF export is a blocking modal; ignore input until it finishes.
    if (exporting_pdf_) return true;
    if (press_modal(x, y)) return true;

    // Pen-preset dropdown swallows presses; the matching release selects a
    // preset or dismisses the menu.
    if (toolbar_.pen_menu_open()) return true;

    if (screen_ == Screen::Browser) {
        // Tap vs. drag-scroll is resolved on release.
        swipe_active_ = true;
        swipe_x0_ = x; swipe_y0_ = y;
        browser_scroll_at_press_ = browser_scroll_;
        return true;
    }

    // When the toolbar is hidden, consume a press on the "show" tab here;
    // the actual show happens on release so the release doesn't land on a
    // freshly-revealed toolbar button (which sits at the same top-right spot
    // and would immediately hide it again).
    if (!toolbar_.visible() && show_tab_rect().contains(x, y)) return true;

    if (y < toolbar_.height()) {
        toolbar_.press(x, y);
        // Repaint only the tapped button — a much smaller e-ink refresh than
        // the whole strip, so press feedback feels immediate.
        Rect br = toolbar_.button_rect_at(x, y);
        if (br.w > 0) redraw_rect(br);
        else          redraw_toolbar();
        return true;
    }
    if (keyboard_.visible() && y >= keyboard_.top_y()) {
        keyboard_.press(x, y);
        redraw_kbd_after_press();
        return true;
    }

    // NoteView canvas (swipe = flip page, tap = follow link) or markdown body
    // (vertical drag = scroll, tap = place cursor). Resolved on release.
    swipe_active_ = true;
    swipe_x0_ = x;
    swipe_y0_ = y;
    md_scroll_at_press_ = markdown_scroll_;
    return true;
}

bool App::press_modal(double x, double y) {
    // The pickers act on press.
    if (picker_.open) { handle_picker_press(x, y);          return true; }
    if (tmpl_open_)   { handle_template_picker_press(x, y); return true; }
    // Draw/OCR modal: drawing is pen-only; its buttons act on release.
    if (draw_open_ || confirm_open_ || settings_open_) return true;
    if (input_open_) {
        // Give the on-screen keyboard its press feedback; buttons act on release.
        if (keyboard_.visible() && y >= keyboard_.top_y()) {
            keyboard_.press(x, y);
            redraw_kbd_after_press();
        }
        return true;
    }
    return false;
}

// ----------------------------------------------------------------- release

bool App::on_button_release(double x, double y) {
    if (exporting_pdf_) return true;
    if (release_modal(x, y)) return true;

    if (screen_ == Screen::Browser) { release_browser(x, y); return true; }

    // swipe_active_ is only set for canvas/body presses, so this can't be
    // confused with toolbar or keyboard taps.
    if (swipe_active_) {
        swipe_active_ = false;
        if (screen_ == Screen::NoteView) release_note_canvas(x, y);
        else                             release_markdown_body(x, y);
        return true;
    }

    // Toolbar hidden: the only chrome is the show-tab. Reveal it here on
    // release (the toolbar is still hidden at this point, so this release
    // can't accidentally trigger a toolbar button).
    if (!toolbar_.visible()) {
        if (show_tab_rect().contains(x, y)) {
            toolbar_.set_visible(true);
            redraw_toolbar();   // only the top strip changes — fast refresh
        }
        return true;
    }

    // The pen dropdown hangs below the toolbar strip, so it's checked before
    // the y < height() gate.
    if (toolbar_.pen_menu_open()) { release_pen_menu(x, y); return true; }
    if (y < toolbar_.height())    { release_toolbar(x, y);  return true; }

    if (keyboard_.visible() && y >= keyboard_.top_y()) {
        // The dispatched key edits the buffer and repaints the text band via
        // handle_key; here we only un-highlight the released key.
        keyboard_.release(x, y);
        redraw_kbd_after_release();
    }
    return true;
}

bool App::release_modal(double x, double y) {
    if (draw_open_) {
        if      (draw_btn_rect(0).contains(x, y)) commit_draw_modal();
        else if (draw_btn_rect(1).contains(x, y)) clear_draw_modal();
        else if (draw_btn_rect(2).contains(x, y)) close_draw_modal();
        return true;
    }
    // The pickers are resolved on press; swallow the release.
    if (picker_.open || tmpl_open_) return true;

    if (confirm_open_) {
        if (modal_btn_rect(true).contains(x, y)) confirm_delete();
        else                                     cancel_confirm();   // Cancel / outside
        return true;
    }
    if (input_open_) {
        if (keyboard_.visible() && y >= keyboard_.top_y()) {
            keyboard_.release(x, y);   // dispatches via handle_key
            redraw_kbd_after_release();
            return true;
        }
        if (input_purpose_ == InputPurpose::Tags &&
            tags_scope_btn_rect().contains(x, y)) {
            toggle_tags_scope();
            return true;
        }
        if      (modal_btn_rect(true).contains(x, y))  commit_input();
        else if (modal_btn_rect(false).contains(x, y)) close_input();
        return true;
    }
    if (settings_open_) {
        if (bt_.open) handle_bluetooth_release(x, y);
        else          handle_settings_release(x, y);
        return true;
    }
    return false;
}

void App::release_browser(double x, double y) {
    // A vertical drag scrolls the grid; a tap dispatches the tile/header
    // action under the finger.
    if (!swipe_active_) return;
    swipe_active_ = false;
    double dx = x - swipe_x0_, dy = y - swipe_y0_;
    if (std::fabs(dy) > kDragMin && std::fabs(dy) >= std::fabs(dx)) {
        browser_scroll_ = browser_scroll_at_press_ - dy;
        clamp_browser_scroll();
        redraw();
        return;
    }
    handle_browser_action(
        browser_.hit(x, y, index_, browser_scroll_, move_active_));
}

void App::release_note_canvas(double x, double y) {
    double dx = x - swipe_x0_, dy = y - swipe_y0_;
    // Page flips are finger-only. The stylus also emits GTK pointer events
    // while drawing, which used to register as horizontal swipes and flip the
    // page mid-stroke. If a pen sample arrived recently, treat this gesture as
    // stylus input and ignore the swipe.
    bool pen_recent = (now_ms() - last_pen_ms_) < kPenSwipeGuardMs;
    if (!pen_recent &&
        std::fabs(dx) > kSwipeMin && std::fabs(dx) > 2.0 * std::fabs(dy)) {
        flip_page(dx < 0 ? 1 : -1);
    } else if (tool_.current == Tool::Pen) {
        follow_link_at(x, y);
    }
}

void App::follow_link_at(double x, double y) {
    const Link *l = link_at(note_, current_page_, x, y);
    if (!l) return;
    std::string resolved = index_.resolve_link(l->target);
    struct stat st;
    if (resolved.empty() || stat(resolved.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        std::string rel = resolved;
        if (rel.rfind(notes_dir_ + "/", 0) == 0)
            rel = rel.substr(notes_dir_.size() + 1);
        enter_note(rel);
    } else {
        enter_markdown(resolved);
    }
}

void App::release_markdown_body(double x, double y) {
    double dx = x - swipe_x0_, dy = y - swipe_y0_;
    if (std::fabs(dy) > kDragMin && std::fabs(dy) >= std::fabs(dx)) {
        // Drag up (dy<0) reveals later content → larger scroll offset.
        markdown_scroll_ = md_scroll_at_press_ - dy;
        clamp_markdown_scroll();
    } else {
        markdown_set_cursor_from_tap(x, y);
    }
    redraw_markdown_body();
}

void App::release_pen_menu(double x, double y) {
    // A width chip updates the size and keeps the menu open (so you can then
    // pick a type with the new width previewed); a type row selects the pen
    // and closes; a tap elsewhere just dismisses.
    PenMenuHit h = toolbar_.pen_menu_hit(x, y);
    if (h.kind == PenMenuHit::Kind::Width) {
        tool_.pen_width_idx = h.index;
        redraw_pen_menu_region();   // keep open, refresh previews
        return;
    }
    if (h.kind == PenMenuHit::Kind::Type) {
        tool_.current  = Tool::Pen;
        tool_.pen_type = kPenTypeMenu[h.index];
    }
    toolbar_.close_pen_menu();
    redraw_pen_menu_region();   // restore page content under the panel
}

void App::release_toolbar(double x, double y) {
    ToolbarAction a = toolbar_.hit(x, y);
    toolbar_.release_all();
    handle_toolbar_action(a);
}

// ------------------------------------------------------- toolbar actions

void App::handle_toolbar_action(ToolbarAction a) {
    // Most actions only change the toolbar/status and get a fast partial
    // refresh. Actions that change the page/screen content set full_redraw.
    bool full_redraw = false;
    switch (a) {
        case ToolbarAction::Pen:
            tool_.current = Tool::Pen;
            toolbar_.toggle_pen_menu();   // reveal/hide the pen dropdown
            // Repaint only the toolbar strip + the narrow dropdown panel.
            redraw_pen_menu_region();
            return;
        case ToolbarAction::Eraser:    tool_.current = Tool::Eraser; break;
        case ToolbarAction::Lasso:     tool_.current = Tool::Lasso;  break;
        case ToolbarAction::Keyboard:
            tool_.keyboard_visible = !tool_.keyboard_visible;
            keyboard_.set_visible(tool_.keyboard_visible);
            full_redraw = true;   // keyboard shows/hides a big region
            break;
        case ToolbarAction::OcrToggle:
            tool_.ocr_enabled = !tool_.ocr_enabled;
            ocr_.set_enabled(tool_.ocr_enabled);
            break;
        case ToolbarAction::Save:
            // Markdown: persist + apply the inline-title rename. Notes: the
            // usual note save. (md auto-saves on navigation too.)
            if (screen_ == Screen::Markdown) flush_markdown();
            else                             save_current();
            break;
        case ToolbarAction::ExportPdf: export_current_pdf(); break;
        case ToolbarAction::AddPage:
            if (screen_ == Screen::NoteView) {
                pages_append(note_, note_.default_template);
                current_page_ = (int)note_.pages.size() - 1;
                note_.mark_dirty();
            }
            full_redraw = true;
            break;
        case ToolbarAction::PrevPage: flip_page(-1); break;
        case ToolbarAction::NextPage: flip_page(+1); break;
        case ToolbarAction::Back:     go_back();       return;
        case ToolbarAction::Browser:  enter_browser(); return;
        case ToolbarAction::Undo:
            if (screen_ == Screen::Markdown) markdown_undo();
            return;   // markdown_undo repaints
        case ToolbarAction::MdView:
            tool_.markdown_pretty = !tool_.markdown_pretty;
            markdown_scroll_ = 0.0;   // line boxes differ between views
            md_lines_.clear();
            full_redraw = true;
            break;
        case ToolbarAction::InsertDrawing:
            if (screen_ == Screen::Markdown) open_draw_modal(DrawPurpose::Image);
            return;   // open_draw_modal repaints
        case ToolbarAction::OcrWord:
            if (screen_ == Screen::Markdown) open_draw_modal(DrawPurpose::Ocr);
            return;
        case ToolbarAction::Template:
            if (screen_ == Screen::NoteView) open_template_picker();
            return;
        case ToolbarAction::Tags:
            open_tags_editor();
            return;   // open_tags_editor shows the modal + redraws
        case ToolbarAction::Hide:
            toolbar_.set_visible(false);
            // Only the toolbar strip changes (toolbar → page content), so a
            // fast partial refresh is enough.
            break;
        case ToolbarAction::Exit:
            quit();
            return;
        default: break;
    }
    if (full_redraw) redraw();
    else             redraw_toolbar();
}

// -------------------------------------------------------- browser actions

void App::clamp_browser_scroll() {
    double content = browser_.content_height(index_);
    double maxs = std::max(0.0, content - (double)win_h_);
    if (browser_scroll_ < 0.0)  browser_scroll_ = 0.0;
    if (browser_scroll_ > maxs) browser_scroll_ = maxs;
}

void App::handle_browser_action(const BrowserHit &h) {
    const IndexEntry *e = nullptr;
    if (h.entry_index >= 0 && h.entry_index < (int)index_.entries().size())
        e = &index_.entries()[h.entry_index];

    switch (h.action) {
        case BrowserAction::NewNote:
            enter_note(index_.create_note("Untitled", TemplateId::Blank).id);
            break;
        case BrowserAction::NewMarkdown:
            enter_markdown(index_.create_markdown("untitled").path);
            break;
        case BrowserAction::NewFolder:
            open_input(InputPurpose::NewFolder, "New folder", "");
            break;
        case BrowserAction::OpenParent:
            enter_parent_folder();
            break;
        case BrowserAction::Open:
            if (!e) break;
            if (e->is_folder)        enter_folder(e->id);
            else if (e->is_markdown) enter_markdown(e->path);
            else                     enter_note(e->id);
            break;
        case BrowserAction::Rename:
            if (e) open_input(InputPurpose::Rename, "Rename", e->title, e->id);
            break;
        case BrowserAction::Delete:
            if (e) open_delete_confirm(*e);
            break;
        case BrowserAction::Move:
            if (!e) break;
            move_active_ = true;
            move_id_     = e->id;
            move_title_  = e->title;
            redraw();
            break;
        case BrowserAction::MoveHere:
            if (!move_active_) break;
            index_.move_entry(move_id_, browser_path_);
            move_active_ = false;
            move_id_.clear();
            move_title_.clear();
            enter_browser();   // rescan + redraw
            break;
        case BrowserAction::MoveCancel:
            move_active_ = false;
            move_id_.clear();
            move_title_.clear();
            redraw();
            break;
        case BrowserAction::Settings:
            open_settings();
            break;
        default: break;
    }
}

bool App::on_motion(double, double) {
    // Pen-driven motion comes via the evdev thread, not GTK.
    return true;
}

} // namespace bn
