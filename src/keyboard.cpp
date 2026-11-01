#include "keyboard.h"

#include "ui.h"

#include <glib.h>

#include <cmath>

namespace bn {

using namespace theme;

namespace {

using Row = std::vector<std::pair<const char *, KeyEvent>>;

// A row of keys whose label is also the text they type.
Row text_row(const std::vector<const char *> &labels) {
    Row r;
    for (const char *l : labels) r.push_back({l, key_text(l)});
    return r;
}

const std::vector<const char *> DIGITS = {"1","2","3","4","5","6","7","8","9","0"};

const std::vector<const char *> US2 = {"q","w","e","r","t","y","u","i","o","p"};
const std::vector<const char *> US3 = {"a","s","d","f","g","h","j","k","l"};
const std::vector<const char *> US4 = {"z","x","c","v","b","n","m","-","'","?"};

// Ukrainian ЙЦУКЕН. The apostrophe and ґ (both everyday letters in
// Ukrainian) get keys of their own on the bottom letter row.
const std::vector<const char *> UA2 = {"й","ц","у","к","е","н","г","ш","щ","з","х","ї"};
const std::vector<const char *> UA3 = {"ф","і","в","а","п","р","о","л","д","ж","є"};
const std::vector<const char *> UA4 = {"я","ч","с","м","и","т","ь","б","ю","'","ґ"};

// Symbols layer. ASCII-only so the Kindle's older Pango never drops a glyph.
const std::vector<const char *> SYM2 = {"@","#","$","%","&","*","-","+","=","/"};
const std::vector<const char *> SYM3 = {"!","?","(",")","[","]","{","}","<",">"};
const std::vector<const char *> SYM4 = {":",";","'","\"","_","|","\\","~","^","`"};

// Markdown formatting row: label differs from the inserted text.
Row md_row() {
    return {
        {"H1", key_text("# ")},   {"H2", key_text("## ")}, {"H3", key_text("### ")},
        {"B",  key_text("**")},   {"I",  key_text("*")},   {"Code", key_text("`")},
        {"List", key_text("- ")}, {"1.", key_text("1. ")}, {"Quote", key_text("> ")},
        {"Link", key_text("[]()")},
    };
}

// Cursor navigation row.
Row nav_row() {
    return {
        {"Home", key_cmd(Key::Home)}, {"Left",  key_cmd(Key::Left)},
        {"Up",   key_cmd(Key::Up)},   {"Down",  key_cmd(Key::Down)},
        {"Right", key_cmd(Key::Right)}, {"End", key_cmd(Key::End)},
    };
}

// Single character that has an upper case (Latin or Cyrillic).
bool is_letter(const std::string &s) {
    if (s.empty() || g_utf8_strlen(s.c_str(), -1) != 1) return false;
    gunichar c = g_utf8_get_char(s.c_str());
    return g_unichar_isalpha(c) && g_unichar_toupper(c) != c;
}

std::string upper(const std::string &s) {
    gchar *u = g_utf8_strup(s.c_str(), -1);
    std::string out(u);
    g_free(u);
    return out;
}

// --- key glyphs drawn as paths (no font dependency) ---------------------

void glyph_shift(cairo_t *cr, double cx, double cy, double s, bool filled) {
    cairo_move_to(cr, cx, cy - s);
    cairo_line_to(cr, cx + s, cy);
    cairo_line_to(cr, cx + s * 0.45, cy);
    cairo_line_to(cr, cx + s * 0.45, cy + s * 0.8);
    cairo_line_to(cr, cx - s * 0.45, cy + s * 0.8);
    cairo_line_to(cr, cx - s * 0.45, cy);
    cairo_line_to(cr, cx - s, cy);
    cairo_close_path(cr);
    if (filled) cairo_fill(cr); else cairo_stroke(cr);
}

void glyph_backspace(cairo_t *cr, double cx, double cy, double s) {
    cairo_move_to(cr, cx - s * 1.2, cy);
    cairo_line_to(cr, cx - s * 0.5, cy - s * 0.7);
    cairo_line_to(cr, cx + s * 1.1, cy - s * 0.7);
    cairo_line_to(cr, cx + s * 1.1, cy + s * 0.7);
    cairo_line_to(cr, cx - s * 0.5, cy + s * 0.7);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - s * 0.05, cy - s * 0.3);
    cairo_line_to(cr, cx + s * 0.55, cy + s * 0.3);
    cairo_move_to(cr, cx + s * 0.55, cy - s * 0.3);
    cairo_line_to(cr, cx - s * 0.05, cy + s * 0.3);
    cairo_stroke(cr);
}

void glyph_enter(cairo_t *cr, double cx, double cy, double s) {
    cairo_move_to(cr, cx + s, cy - s * 0.7);
    cairo_line_to(cr, cx + s, cy + s * 0.2);
    cairo_line_to(cr, cx - s, cy + s * 0.2);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - s * 0.5, cy - s * 0.3);
    cairo_line_to(cr, cx - s, cy + s * 0.2);
    cairo_line_to(cr, cx - s * 0.5, cy + s * 0.7);
    cairo_stroke(cr);
}

constexpr double kGap    = 6.0;   // between keys
constexpr double kMargin = 8.0;   // tray edge
constexpr double kSpecialFill = 0.84;

} // namespace

void Keyboard::add_row(const Row &row, double y, Look look) {
    int n = (int)row.size();
    double kw = (w_ - 2 * kMargin - (n - 1) * kGap) / (double)n;
    for (int i = 0; i < n; ++i) {
        Cap k;
        k.r     = Rect{kMargin + i * (kw + kGap), y, kw, kh_ - kGap};
        k.label = row[i].first;
        k.out   = row[i].second;
        k.look  = look;
        keys_.push_back(k);
    }
}

void Keyboard::layout(double w, double h) {
    w_ = w; h_ = h;
    const int rows = 7;          // markdown + digits + 3 letter rows + nav + bottom
    top_y_ = h - (rows * kh_ + 14);
    keys_.clear();

    auto row_y = [&](int i) { return top_y_ + 10 + i * kh_; };
    add_row(md_row(), row_y(0), Look::Format);
    add_row(text_row(DIGITS), row_y(1), Look::Char);
    if (mode_ == Mode::Symbols) {
        add_row(text_row(SYM2), row_y(2), Look::Char);
        add_row(text_row(SYM3), row_y(3), Look::Char);
        add_row(text_row(SYM4), row_y(4), Look::Char);
    } else if (lang_ == Layout::UA) {
        add_row(text_row(UA2), row_y(2), Look::Char);
        add_row(text_row(UA3), row_y(3), Look::Char);
        add_row(text_row(UA4), row_y(4), Look::Char);
    } else {
        add_row(text_row(US2), row_y(2), Look::Char);
        add_row(text_row(US3), row_y(3), Look::Char);
        add_row(text_row(US4), row_y(4), Look::Char);
    }
    add_row(nav_row(), row_y(5), Look::Format);

    // Bottom row, laid out by relative weight. Shift only applies to letters,
    // so the symbols layer drops it and Space grows.
    struct Slot { double weight; const char *label; Kind kind; Look look; KeyEvent out; };
    std::vector<Slot> slots;
    bool letters = mode_ == Mode::Letters;
    slots.push_back({1.3, letters ? "?123" : "ABC", Kind::ModeSwitch, Look::Special, {}});
    if (letters) slots.push_back({1.3, "Shift", Kind::Shift, Look::Special, {}});
    slots.push_back({1.2, layout_badge(lang_), Kind::Emit, Look::Special,
                     key_cmd(Key::SwitchLayout)});
    slots.push_back({0.8, ",", Kind::Emit, Look::Char, key_text(",")});
    slots.push_back({letters ? 3.4 : 4.7, "", Kind::Emit, Look::Char, key_text(" ")});
    slots.push_back({0.8, ".", Kind::Emit, Look::Char, key_text(".")});
    slots.push_back({1.5, "Bksp", Kind::Emit, Look::Special, key_cmd(Key::Backspace)});
    slots.push_back({1.9, "Enter", Kind::Emit, Look::Accent, key_cmd(Key::Enter)});

    double total = 0;
    for (auto &s : slots) total += s.weight;
    double avail = w_ - 2 * kMargin - (slots.size() - 1) * kGap;
    double x = kMargin, y = row_y(6);
    for (auto &s : slots) {
        Cap k;
        k.r     = Rect{x, y, avail * s.weight / total, kh_ - kGap};
        k.label = s.label;
        k.kind  = s.kind;
        k.look  = s.look;
        k.out   = s.out;
        keys_.push_back(k);
        x += k.r.w + kGap;
    }
}

void Keyboard::draw_cap(cairo_t *cr, const Cap &k, bool down) const {
    bool shift_on = k.kind == Kind::Shift && shift_;
    double fill = kPaper;
    if (k.look == Look::Special || k.look == Look::Format) fill = kSpecialFill;
    if (k.look == Look::Accent || shift_on) fill = kInk;
    if (down) fill = (fill == kInk) ? 0.35 : kInk;
    double fg = (fill < 0.5) ? kPaper : kInk;

    // A darker lip under the key face, kept inside the key's own rect so a
    // single-key repaint covers all of it.
    Rect face{k.r.x, k.r.y, k.r.w, k.r.h - 3};
    if (!down) ui_fill_round(cr, Rect{k.r.x, k.r.y + 3, k.r.w, k.r.h - 3}, kRadiusSm, kShadow);
    ui_fill_round(cr, down ? Rect{k.r.x, k.r.y + 3, k.r.w, k.r.h - 3} : face,
                  kRadiusSm, fill);
    if (down) face.y += 3;

    double cx = face.x + face.w / 2.0, cy = face.y + face.h / 2.0;
    ui_grey(cr, fg);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (k.kind == Kind::Shift) { glyph_shift(cr, cx, cy, 13, shift_on); return; }
    if (k.out.key == Key::Backspace) { glyph_backspace(cr, cx, cy, 13); return; }
    if (k.out.key == Key::Enter)     { glyph_enter(cr, cx, cy, 14); return; }
    if (k.out.key == Key::SwitchLayout) {
        int tw, th; ui_text_size(cr, k.label, "Sans Bold 17", &tw, &th);
        double gx = cx - (tw + 34) / 2.0 + 12;
        ui_icon(cr, Icon::Globe, gx, cy, 12);
        ui_text(cr, gx + 22, cy - th / 2.0, k.label, "Sans Bold 17", fg);
        return;
    }
    if (k.out.key == Key::Text && k.out.text == " ") {   // space bar
        ui_text_centered(cr, face, lang_ == Layout::UA ? "пробіл" : "space",
                         "Sans 16", fill < 0.5 ? kPaper : kMuted);
        return;
    }

    std::string lab = k.label;
    if (shift_ && k.look == Look::Char && is_letter(lab)) lab = upper(lab);
    const char *font = k.look == Look::Char ? "Sans 24"
                     : k.look == Look::Format ? "Sans 17" : "Sans Bold 17";
    ui_text_centered(cr, face, lab, font, fg);
}

void Keyboard::draw(cairo_t *cr) {
    if (!visible_) return;
    ui_grey(cr, kSurface);
    cairo_rectangle(cr, 0, top_y_, w_, h_ - top_y_); cairo_fill(cr);
    ui_hline(cr, 0, w_, top_y_, kLine);

    for (size_t i = 0; i < keys_.size(); ++i)
        draw_cap(cr, keys_[i], (int)i == pressed_idx_);
}

bool Keyboard::press(double x, double y) {
    if (!visible_ || y < top_y_) return false;
    last_key_rect_ = Rect{};
    last_full_redraw_ = false;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i].r.contains(x, y)) {
            pressed_idx_ = (int)i;
            last_key_rect_ = keys_[i].r;
            return true;
        }
    }
    return true;  // consume taps in the keyboard band
}

bool Keyboard::release(double x, double y) {
    if (!visible_ || pressed_idx_ < 0) return false;
    // Always un-highlight the key we pressed, even on an off-key release.
    last_key_rect_    = keys_[pressed_idx_].r;
    last_full_redraw_ = false;
    if (!keys_[pressed_idx_].r.contains(x, y)) {
        pressed_idx_ = -1;
        return true;
    }
    const Cap k = keys_[pressed_idx_];
    pressed_idx_ = -1;

    switch (k.kind) {
    case Kind::ModeSwitch:
        mode_  = (mode_ == Mode::Letters) ? Mode::Symbols : Mode::Letters;
        shift_ = false;
        last_full_redraw_ = true;     // every key changed
        return true;
    case Kind::Shift:
        shift_ = !shift_;
        last_full_redraw_ = true;     // letter labels flip case
        return true;
    case Kind::Emit:
        break;
    }

    KeyEvent out = k.out;
    if (out.key == Key::SwitchLayout) {
        last_full_redraw_ = true;     // the letter rows change
        mode_ = Mode::Letters;
    }
    if (shift_ && out.key == Key::Text && is_letter(out.text)) {
        out.text = upper(out.text);
        shift_ = false;
        last_full_redraw_ = true;     // shift was consumed → labels revert
    }
    if (key_cb_) key_cb_(out);
    return true;
}

} // namespace bn
