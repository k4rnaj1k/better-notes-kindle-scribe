// Keyboard input. Three sources feed handle_key():
//   - the on-screen Cairo keyboard (KeyEvent::hardware = false)
//   - Bluetooth / USB keyboards read from evdev by hid_ (kindle-hid-passthrough
//     exposes paired keyboards as /dev/input/eventN)
//   - keyboards the X server adopted itself, via GTK key-press-event
// Modals get first pick, then the active screen. Single-letter shortcuts only
// fire for hardware keys, so a tap on the on-screen keyboard never switches
// tools.

#include "app.h"

#include "pages.h"

#include <gdk/gdkkeysyms.h>

#include <algorithm>

namespace bn {

namespace {

// Hardware key with no Ctrl/Alt that typed exactly `c`.
bool plain_char(const KeyEvent &e, char c) {
    return e.hardware && e.key == Key::Text && !e.ctrl() && !e.alt() &&
           e.text.size() == 1 && e.text[0] == c;
}

Key gdk_command(guint keyval) {
    switch (keyval) {
    case GDK_Return: case GDK_KP_Enter:   return Key::Enter;
    case GDK_BackSpace:                   return Key::Backspace;
    case GDK_Delete: case GDK_KP_Delete:  return Key::Delete;
    case GDK_Tab: case GDK_ISO_Left_Tab:  return Key::Tab;
    case GDK_Escape:                      return Key::Escape;
    case GDK_Left:  case GDK_KP_Left:     return Key::Left;
    case GDK_Right: case GDK_KP_Right:    return Key::Right;
    case GDK_Up:    case GDK_KP_Up:       return Key::Up;
    case GDK_Down:  case GDK_KP_Down:     return Key::Down;
    case GDK_Home:  case GDK_KP_Home:     return Key::Home;
    case GDK_End:   case GDK_KP_End:      return Key::End;
    case GDK_Page_Up:   case GDK_KP_Page_Up:   return Key::PageUp;
    case GDK_Page_Down: case GDK_KP_Page_Down: return Key::PageDown;
    case GDK_F2:                          return Key::F2;
    default:                              return Key::None;
    }
}

} // namespace

// ------------------------------------------------------------- sources

bool App::on_gtk_key(GdkEventKey *ev) {
    // While we read keyboards straight from evdev, X should not be getting
    // their keys at all (we EVIOCGRAB them). If it still does, drop them here
    // so nothing is typed twice.
    if (hid_.device_count() > 0) return false;

    KeyEvent e;
    e.hardware = true;
    if (ev->state & GDK_SHIFT_MASK)   e.mods |= kModShift;
    if (ev->state & GDK_CONTROL_MASK) e.mods |= kModCtrl;
    if (ev->state & GDK_MOD1_MASK)    e.mods |= kModAlt;

    e.key = gdk_command(ev->keyval);
    if (e.key == Key::None) {
        // Shortcuts match on the base key, so Ctrl+Shift+N still reads "n".
        guint kv = (e.ctrl() || e.alt()) ? gdk_keyval_to_lower(ev->keyval)
                                         : ev->keyval;
        gunichar uc = gdk_keyval_to_unicode(kv);
        if (uc < 0x20 || uc == 0x7f) return false;   // modifiers, dead keys…
        char buf[8];
        int n = g_unichar_to_utf8(uc, buf);
        e.key  = Key::Text;
        e.text = std::string(buf, n);
    }
    if (!x_keys_seen_) {
        x_keys_seen_ = true;
        log_info("kbd: receiving keys through X");
        if (screen_ == Screen::Browser && browser_.focus() < 0) {
            browser_.set_focus(0);
            redraw();
        }
    }
    handle_key(e);
    return true;
}

void App::on_hw_keyboard(const std::string &name, bool added) {
    status_ = (added ? "keyboard: " : "keyboard gone: ") + name;
    log_info("kbd: %s %s", added ? "connected" : "disconnected", name.c_str());
    // The on-screen keyboard is only needed in a text modal without a
    // physical one.
    if (input_open_) keyboard_.set_visible(!hw_keyboard());
    if (screen_ == Screen::Browser) {
        int cells = browser_.cell_count(index_);
        browser_.set_focus(hw_keyboard() && cells > 0
                               ? std::max(0, std::min(browser_.focus(), cells - 1))
                               : -1);
    }
    redraw();
}

void App::set_kbd_layout(Layout l) {
    layout_ = l;
    hid_.set_layout(l);
    keyboard_.set_layout(l);
    status_ = std::string("layout: ") + layout_badge(l);
    save_config();
    // Repaint only what shows the layout: the keyboard tray, the open text
    // dialog, the toolbar badge, the settings switch.
    if (keyboard_.visible()) redraw_keyboard_band();
    if (input_open_)         redraw_input_card();
    if (settings_open_)      redraw_rect(settings_card_rect());
    if (screen_ != Screen::Browser) redraw_toolbar();
}

// ------------------------------------------------------------- routing

void App::handle_key(const KeyEvent &e) {
    // PDF export pumps the main loop; ignore keys until it's done.
    if (exporting_pdf_) return;
    // Layout switching works everywhere, including inside dialogs.
    if (e.key == Key::SwitchLayout) { set_kbd_layout(next_layout(layout_)); return; }

    if (picker_.open || tmpl_open_) { key_list_modal(e);    return; }
    if (draw_open_)                 { key_draw_modal(e);    return; }
    if (input_open_)                { key_input_modal(e);   return; }
    if (confirm_open_)              { key_confirm_modal(e); return; }
    if (settings_open_) {
        if (bt_.open)                  key_bluetooth(e);
        else if (e.key == Key::Escape) close_settings();
        return;
    }
    if (toolbar_.pen_menu_open() && e.key == Key::Escape) {
        toolbar_.close_pen_menu();
        redraw_pen_menu_region();
        return;
    }
    if (key_global(e)) return;

    switch (screen_) {
    case Screen::Browser:  key_browser(e);  break;
    case Screen::NoteView: key_note(e);     break;
    case Screen::Markdown: key_markdown(e); break;
    }
}

bool App::key_global(const KeyEvent &e) {
    if (!e.hardware) return false;
    if (e.is_ctrl('q')) { quit(); return true; }
    if (e.is_ctrl('s') && screen_ != Screen::Browser) {
        handle_toolbar_action(ToolbarAction::Save);
        return true;
    }
    return false;
}

// -------------------------------------------------------------- modals

bool App::key_input_modal(const KeyEvent &e) {
    switch (e.key) {
    case Key::Enter:  commit_input(); return true;
    case Key::Escape: close_input();  return true;
    case Key::Tab:
        if (input_purpose_ == InputPurpose::Tags) toggle_tags_scope();
        return true;
    default:
        break;
    }
    if (input_.apply(e)) { redraw_input_card(); return true; }
    return false;
}

bool App::key_confirm_modal(const KeyEvent &e) {
    if (e.key == Key::Enter || plain_char(e, 'y')) { confirm_delete(); return true; }
    if (e.key == Key::Escape || plain_char(e, 'n')) { cancel_confirm(); return true; }
    return false;
}

bool App::key_draw_modal(const KeyEvent &e) {
    if (e.key == Key::Escape) { close_draw_modal(); return true; }
    if (e.key == Key::Enter || e.is_ctrl('s')) { commit_draw_modal(); return true; }
    if (e.key == Key::Backspace || e.key == Key::Delete) { clear_draw_modal(); return true; }
    return false;
}

// ------------------------------------------------------------- browser

bool App::key_browser(const KeyEvent &e) {
    int cells = browser_.cell_count(index_);
    int cols  = std::max(1, browser_.columns());
    int f     = browser_.focus();

    auto focus = [&](int nf) {
        if (cells <= 0) return;
        nf = std::clamp(nf, 0, cells - 1);
        browser_.set_focus(nf);
        browser_scroll_ = browser_.scroll_to_show(nf, browser_scroll_);
        clamp_browser_scroll();
        redraw();
    };
    auto act = [&](BrowserAction a) {
        int entry = f >= 0 ? browser_.entry_for_cell(f) : -1;
        handle_browser_action({a, entry});
    };

    if (e.key == Key::Escape && move_active_) { act(BrowserAction::MoveCancel); return true; }
    if (e.key == Key::Enter && e.ctrl() && move_active_) { act(BrowserAction::MoveHere); return true; }
    if (e.is_ctrl('n')) {
        act(e.shift() ? BrowserAction::NewFolder : BrowserAction::NewNote);
        return true;
    }
    if (e.is_ctrl('m')) { act(BrowserAction::NewMarkdown); return true; }
    if (e.key == Key::Backspace || (e.alt() && e.key == Key::Up)) {
        enter_parent_folder();
        return true;
    }

    // Navigation. The first arrow press with no focus just shows the ring.
    bool nav = e.key == Key::Left || e.key == Key::Right || e.key == Key::Up ||
               e.key == Key::Down || e.key == Key::Home || e.key == Key::End ||
               e.key == Key::PageUp || e.key == Key::PageDown;
    if (nav && f < 0) { focus(0); return true; }
    switch (e.key) {
    case Key::Left:     focus(f - 1);        return true;
    case Key::Right:    focus(f + 1);        return true;
    case Key::Up:       focus(f - cols);     return true;
    case Key::Down:     focus(f + cols);     return true;
    case Key::Home:     focus(0);            return true;
    case Key::End:      focus(cells - 1);    return true;
    case Key::PageUp:   focus(f - 2 * cols); return true;
    case Key::PageDown: focus(f + 2 * cols); return true;
    default: break;
    }
    if (f < 0) return false;

    int entry = browser_.entry_for_cell(f);
    const IndexEntry *ent = (entry >= 0 && entry < (int)index_.entries().size())
                                ? &index_.entries()[entry] : nullptr;
    if (e.key == Key::Enter) {
        handle_browser_action(browser_.activate(f, index_, move_active_));
        return true;
    }
    if (!ent || move_active_) return false;
    if (e.key == Key::F2)                       { act(BrowserAction::Rename); return true; }
    if (e.key == Key::Delete && !ent->is_folder) { act(BrowserAction::Delete); return true; }
    if (plain_char(e, 'm'))                     { act(BrowserAction::Move);   return true; }
    return false;
}

// ---------------------------------------------------------------- note

bool App::key_note(const KeyEvent &e) {
    switch (e.key) {
    case Key::Left: case Key::PageUp:    flip_page(-1); return true;
    case Key::Right: case Key::PageDown: flip_page(+1); return true;
    case Key::Home: flip_page(-current_page_); return true;
    case Key::End:  flip_page((int)note_.pages.size()); return true;
    case Key::Escape: go_back(); return true;
    default: break;
    }
    if (!e.hardware) return false;

    if (e.is_ctrl('n')) { handle_toolbar_action(ToolbarAction::AddPage);   return true; }
    if (e.is_ctrl('e')) { handle_toolbar_action(ToolbarAction::ExportPdf); return true; }
    if (e.is_ctrl('t')) { handle_toolbar_action(ToolbarAction::Template);  return true; }

    auto set_tool = [&](Tool t) { tool_.current = t; redraw_toolbar(); };
    if (plain_char(e, 'p')) { set_tool(Tool::Pen);    return true; }
    if (plain_char(e, 'e')) { set_tool(Tool::Eraser); return true; }
    if (plain_char(e, 'l')) { set_tool(Tool::Lasso);  return true; }
    if (plain_char(e, 't')) { open_tags_editor();     return true; }
    if (plain_char(e, 'h')) {
        toolbar_.set_visible(!toolbar_.visible());
        redraw_toolbar();
        return true;
    }
    // 1..5 pick the pen width (XS..XL).
    if (e.key == Key::Text && !e.ctrl() && e.text.size() == 1 &&
        e.text[0] >= '1' && e.text[0] < '1' + kPenWidthCount) {
        tool_.pen_width_idx = e.text[0] - '1';
        tool_.current = Tool::Pen;
        redraw_toolbar();
        return true;
    }
    return false;
}

// ------------------------------------------------------------ markdown

bool App::key_markdown(const KeyEvent &e) {
    if (e.key == Key::Escape) { go_back(); return true; }

    if (e.hardware) {
        if (e.is_ctrl('z')) { markdown_undo(); return true; }
        if (e.is_ctrl('e')) { handle_toolbar_action(ToolbarAction::MdView); return true; }
        if (e.is_ctrl('b')) { markdown_wrap("**"); redraw_markdown_body(); return true; }
        if (e.is_ctrl('i')) { markdown_wrap("*");  redraw_markdown_body(); return true; }
        if (e.is_ctrl('k')) {
            // "[|]()" — cursor inside the brackets, ready for the link text.
            markdown_edit(key_text("[]()"));
            md_.cursor -= 3;
            redraw_markdown_body();
            return true;
        }
    }

    switch (e.key) {
    case Key::Enter:
        markdown_newline();
        break;
    case Key::Tab:
        markdown_edit(key_text("    "));
        break;
    case Key::Up:
    case Key::Down:
        if (tool_.markdown_pretty) {
            // No caret in the preview; arrows scroll instead.
            markdown_scroll_ += (e.key == Key::Up ? -60.0 : 60.0);
            clamp_markdown_scroll();
        } else {
            markdown_move_cursor_vertical(e.key == Key::Up ? -1 : 1);
        }
        break;
    case Key::PageUp:   markdown_page(-1); return true;
    case Key::PageDown: markdown_page(+1); return true;
    default:
        if (e.key == Key::Text && (e.ctrl() || e.alt())) return false;
        markdown_edit(e);
        break;
    }
    redraw_markdown_body();
    return true;
}

} // namespace bn
