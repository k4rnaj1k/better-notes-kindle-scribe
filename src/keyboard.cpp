#include "keyboard.h"

#include <pango/pangocairo.h>

namespace bn {

namespace {

const char *ROW1[] = {"1","2","3","4","5","6","7","8","9","0"};
const char *ROW2[] = {"q","w","e","r","t","y","u","i","o","p"};
const char *ROW3[] = {"a","s","d","f","g","h","j","k","l"};
const char *ROW4[] = {"z","x","c","v","b","n","m",",",".","?"};

// Symbols layer. ASCII-only so the Kindle's older Pango never drops a glyph.
const char *SROW1[] = {"1","2","3","4","5","6","7","8","9","0"};
const char *SROW2[] = {"@","#","$","%","&","*","-","+","=","/"};
const char *SROW3[] = {"!","?","(",")","[","]","{","}","<",">"};
const char *SROW4[] = {":",";","'","\"",",",".","_","|","\\","~"};

// Markdown formatting row: label differs from the inserted text. Labels are
// kept ASCII so the Kindle's older Pango never drops a glyph.
struct MdKey { const char *label; const char *out; };
const MdKey MDROW[] = {
    {"H1","# "}, {"H2","## "}, {"H3","### "}, {"B","**"},   {"I","*"},
    {"Code","`"},{"List","- "},{"1.","1. "},  {"Quote","> "},{"Link","[]()"},
};

} // namespace

void Keyboard::layout(double w, double h) {
    w_ = w; h_ = h;
    double kh    = 80.0;
    double rows  = 7;          // markdown + nav + 4 letter rows + space row
    double total_h = rows * kh + 8;
    top_y_ = h - total_h;
    keys_.clear();

    double margin = 8.0;
    auto add_row = [&](const char **labels, int n, double y) {
        double kw = (w_ - 2 * margin - (n - 1) * 4) / (double)n;
        for (int i = 0; i < n; ++i) {
            Key k;
            k.r.x = margin + i * (kw + 4);
            k.r.y = y;
            k.r.w = kw;
            k.r.h = kh - 6;
            k.label  = labels[i];
            k.output = labels[i];
            keys_.push_back(k);
        }
    };

    // Markdown formatting row at the top.
    {
        int n = (int)(sizeof(MDROW) / sizeof(MDROW[0]));
        double kw = (w_ - 2 * margin - (n - 1) * 4) / (double)n;
        double y = top_y_ + 4;
        for (int i = 0; i < n; ++i) {
            Key k;
            k.r.x = margin + i * (kw + 4);
            k.r.y = y;
            k.r.w = kw;
            k.r.h = kh - 6;
            k.label  = MDROW[i].label;
            k.output = MDROW[i].out;
            keys_.push_back(k);
        }
    }

    if (mode_ == Mode::Symbols) {
        add_row(SROW1, 10, top_y_ + 4 + kh);
        add_row(SROW2, 10, top_y_ + 4 + 2 * kh);
        add_row(SROW3, 10, top_y_ + 4 + 3 * kh);
        add_row(SROW4, 10, top_y_ + 4 + 4 * kh);
    } else {
        add_row(ROW1, 10, top_y_ + 4 + kh);
        add_row(ROW2, 10, top_y_ + 4 + 2 * kh);
        add_row(ROW3, 9,  top_y_ + 4 + 3 * kh);
        add_row(ROW4, 10, top_y_ + 4 + 4 * kh);
    }

    // Cursor navigation row. Labels are ASCII words so the Kindle's older
    // Pango never drops a glyph; outputs match App's markdown key handler.
    {
        const char *nav[] = {"Home", "Left", "Up", "Down", "Right", "End"};
        int n = (int)(sizeof(nav) / sizeof(nav[0]));
        double kw = (w_ - 2 * margin - (n - 1) * 4) / (double)n;
        double y = top_y_ + 4 + 5 * kh;
        for (int i = 0; i < n; ++i) {
            Key k;
            k.r.x = margin + i * (kw + 4);
            k.r.y = y;
            k.r.w = kw;
            k.r.h = kh - 6;
            k.label  = nav[i];
            k.output = nav[i];
            keys_.push_back(k);
        }
    }

    // Last row. A Mode key (?123 ↔ ABC) toggles the symbols layer; Shift is
    // only meaningful for letters, so it's dropped in symbols mode and Space
    // grows to fill the gap.
    double y = top_y_ + 4 + 6 * kh;
    double remain = w_ - 2 * margin;
    auto frac = [&](double fx, double fw) {
        return Rect{margin + remain * fx, y, remain * fw, kh - 6};
    };
    if (mode_ == Mode::Symbols) {
        keys_.push_back(Key{frac(0.00, 0.15), "ABC",   "Mode"});
        keys_.push_back(Key{frac(0.16, 0.46), "Space", " "});
        keys_.push_back(Key{frac(0.63, 0.17), "Bksp",  "Backspace"});
        keys_.push_back(Key{frac(0.81, 0.19), "Enter", "Enter"});
    } else {
        keys_.push_back(Key{frac(0.00, 0.13), "?123",  "Mode"});
        keys_.push_back(Key{frac(0.14, 0.13), "Shift", "Shift"});
        keys_.push_back(Key{frac(0.28, 0.34), "Space", " "});
        keys_.push_back(Key{frac(0.63, 0.17), "Bksp",  "Backspace"});
        keys_.push_back(Key{frac(0.81, 0.19), "Enter", "Enter"});
    }
}

void Keyboard::draw(cairo_t *cr) {
    if (!visible_) return;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
    cairo_rectangle(cr, 0, top_y_, w_, h_ - top_y_); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, top_y_); cairo_line_to(cr, w_, top_y_);
    cairo_stroke(cr);

    PangoFontDescription *fd = pango_font_description_from_string("Sans 22");
    for (size_t i = 0; i < keys_.size(); ++i) {
        const Key &k = keys_[i];
        bool down = (int)i == pressed_idx_;
        cairo_set_source_rgb(cr,
            down ? 0.60 : 0.99,
            down ? 0.60 : 0.99,
            down ? 0.60 : 0.99);
        cairo_rectangle(cr, k.r.x, k.r.y, k.r.w, k.r.h); cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_rectangle(cr, k.r.x, k.r.y, k.r.w, k.r.h); cairo_stroke(cr);

        std::string lab = k.label;
        if (shift_ && lab.size() == 1 && lab[0] >= 'a' && lab[0] <= 'z')
            lab[0] = (char)(lab[0] - 32);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, fd);
        pango_layout_set_text(layout, lab.c_str(), -1);
        int tw, th; pango_layout_get_size(layout, &tw, &th);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr,
            k.r.x + (k.r.w - tw / (double)PANGO_SCALE) / 2.0,
            k.r.y + (k.r.h - th / (double)PANGO_SCALE) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    pango_font_description_free(fd);
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
    const Key &k = keys_[pressed_idx_];
    pressed_idx_ = -1;

    if (k.output == "Mode") {
        mode_ = (mode_ == Mode::Letters) ? Mode::Symbols : Mode::Letters;
        shift_ = false;
        last_full_redraw_ = true;     // every key changed
    } else if (k.output == "Shift") {
        shift_ = !shift_;
        last_full_redraw_ = true;     // letter labels flip case
    } else if (k.output == "Backspace" || k.output == "Enter" ||
               k.output == "Left" || k.output == "Right" ||
               k.output == "Up"   || k.output == "Down"  ||
               k.output == "Home" || k.output == "End") {
        if (key_cb_) key_cb_(k.output);
    } else if (k.output == " ") {
        if (text_cb_) text_cb_(" ");
    } else {
        std::string out = k.output;
        if (shift_ && out.size() == 1 && out[0] >= 'a' && out[0] <= 'z') {
            out[0] = (char)(out[0] - 32);
            shift_ = false;
            last_full_redraw_ = true; // shift was consumed → labels revert
        }
        if (text_cb_) text_cb_(out);
    }
    return true;
}

} // namespace bn
