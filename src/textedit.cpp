#include "textedit.h"

#include <cctype>

namespace bn {

namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Word characters for Ctrl+arrow jumps. Any non-ASCII byte counts as part of
// a word so accented / non-Latin words move as a unit.
bool is_word(unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '_'; }

} // namespace

size_t utf8_prev(const std::string &s, size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    --i;
    while (i > 0 && is_cont((unsigned char)s[i])) --i;
    return i;
}

size_t utf8_next(const std::string &s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && is_cont((unsigned char)s[i])) ++i;
    return i;
}

void TextEdit::insert(const std::string &s) {
    clamp();
    text.insert(cursor, s);
    cursor += s.size();
}

bool TextEdit::backspace() {
    clamp();
    if (cursor == 0) return false;
    size_t p = utf8_prev(text, cursor);
    text.erase(p, cursor - p);
    cursor = p;
    return true;
}

bool TextEdit::delete_forward() {
    clamp();
    if (cursor >= text.size()) return false;
    size_t n = utf8_next(text, cursor);
    text.erase(cursor, n - cursor);
    return true;
}

void TextEdit::left()  { clamp(); cursor = utf8_prev(text, cursor); }
void TextEdit::right() { clamp(); cursor = utf8_next(text, cursor); }

void TextEdit::word_left() {
    clamp();
    while (cursor > 0 && !is_word((unsigned char)text[cursor - 1])) --cursor;
    while (cursor > 0 &&  is_word((unsigned char)text[cursor - 1])) --cursor;
}

void TextEdit::word_right() {
    clamp();
    while (cursor < text.size() && !is_word((unsigned char)text[cursor])) ++cursor;
    while (cursor < text.size() &&  is_word((unsigned char)text[cursor])) ++cursor;
}

void TextEdit::line_home() {
    clamp();
    size_t nl = cursor == 0 ? std::string::npos : text.rfind('\n', cursor - 1);
    cursor = (nl == std::string::npos) ? 0 : nl + 1;
}

void TextEdit::line_end() {
    clamp();
    size_t nl = text.find('\n', cursor);
    cursor = (nl == std::string::npos) ? text.size() : nl;
}

bool TextEdit::apply(const KeyEvent &e, bool *changed) {
    bool ch = false;
    switch (e.key) {
    case Key::Text:
        if (e.ctrl() || e.alt()) return false;   // shortcuts aren't text
        insert(e.text); ch = true;
        break;
    case Key::Backspace: ch = backspace();      break;
    case Key::Delete:    ch = delete_forward(); break;
    case Key::Left:  e.ctrl() ? word_left()  : left();      break;
    case Key::Right: e.ctrl() ? word_right() : right();     break;
    case Key::Home:  e.ctrl() ? doc_home()   : line_home(); break;
    case Key::End:   e.ctrl() ? doc_end()    : line_end();  break;
    default: return false;
    }
    if (changed) *changed = ch;
    return true;
}

} // namespace bn
