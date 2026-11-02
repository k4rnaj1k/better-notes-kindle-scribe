#pragma once
// One key event, shared by every keyboard source: the on-screen Cairo
// keyboard, a Bluetooth keyboard read straight from evdev (hidkbd), and the
// X/GTK key path. App::handle_key is the single consumer, so text entry and
// shortcuts behave the same no matter where the key came from.

#include <cstdint>
#include <string>

namespace bn {

enum class Key {
    None,
    Text,        // printable input; KeyEvent::text holds the UTF-8
    Enter,
    Backspace,
    Delete,      // forward delete
    Tab,
    Escape,
    Left, Right, Up, Down,
    Home, End,
    PageUp, PageDown,
    F2,          // browser: rename
    SwitchLayout,  // Ctrl+Space / Alt+Shift, or the on-screen language key
};

// Text layouts for the evdev reader and the on-screen keyboard. (Keys that
// arrive through X use whatever layout X is configured with.)
enum class Layout { US, UA };
constexpr int kLayoutCount = 2;

inline const char *layout_badge(Layout l) { return l == Layout::UA ? "UA" : "EN"; }
inline const char *layout_id(Layout l)    { return l == Layout::UA ? "ua" : "us"; }
inline Layout layout_from_id(const std::string &id) {
    return id == "ua" || id == "uk" ? Layout::UA : Layout::US;
}
inline Layout next_layout(Layout l) {
    return (Layout)(((int)l + 1) % kLayoutCount);
}

enum KeyMod : uint8_t {
    kModNone  = 0,
    kModShift = 1 << 0,
    kModCtrl  = 1 << 1,
    kModAlt   = 1 << 2,
};

struct KeyEvent {
    Key         key  = Key::None;
    std::string text;            // only for Key::Text
    uint8_t     mods = kModNone;
    // true for a physical keyboard (evdev or X). The on-screen keyboard sets
    // false so single-letter shortcuts never fire from an on-screen tap.
    bool        hardware = false;

    bool ctrl()  const { return mods & kModCtrl; }
    bool shift() const { return mods & kModShift; }
    bool alt()   const { return mods & kModAlt; }
    // Ctrl+<letter>, case-insensitive. Text carries the unshifted letter.
    bool is_ctrl(char c) const {
        return ctrl() && key == Key::Text && text.size() == 1 &&
               (text[0] == c || text[0] == c - 32);
    }
};

inline KeyEvent key_text(const std::string &t, bool hw = false) {
    KeyEvent e; e.key = Key::Text; e.text = t; e.hardware = hw; return e;
}
inline KeyEvent key_cmd(Key k, bool hw = false) {
    KeyEvent e; e.key = k; e.hardware = hw; return e;
}

} // namespace bn
