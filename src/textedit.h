#pragma once
// A UTF-8 text buffer with a byte-offset cursor. Backs both the markdown
// editor and the single-line input modal (rename / tags / paths), so cursor
// movement and deletion never split a multi-byte character, and the on-screen
// and hardware keyboards edit through the same code.

#include "keys.h"

#include <cstddef>
#include <string>
#include <utility>

namespace bn {

struct TextEdit {
    std::string text;
    size_t      cursor = 0;   // byte offset, always on a codepoint boundary

    void set(std::string t) { text = std::move(t); cursor = text.size(); }
    void clear()            { text.clear(); cursor = 0; }
    void clamp()            { if (cursor > text.size()) cursor = text.size(); }

    void insert(const std::string &s);
    bool backspace();        // false if nothing was deleted
    bool delete_forward();

    void left();
    void right();
    void word_left();
    void word_right();
    void line_home();        // start of the current '\n'-delimited line
    void line_end();
    void doc_home() { cursor = 0; }
    void doc_end()  { cursor = text.size(); }

    // Apply the editing/navigation part of a key event: Text, Backspace,
    // Delete, Left/Right (Ctrl = by word), Home/End (Ctrl = whole buffer).
    // Enter/Tab/Up/Down are left to the caller, which knows whether this is a
    // single-line field or the multi-line editor. Returns true if handled.
    bool apply(const KeyEvent &e, bool *changed = nullptr);
};

// Codepoint-boundary helpers for UTF-8 byte offsets.
size_t utf8_prev(const std::string &s, size_t i);
size_t utf8_next(const std::string &s, size_t i);

} // namespace bn
