#include "keyboard.h"

#include <pango/pangocairo.h>

namespace bn {

namespace {

const char *ROW1[] = {"1","2","3","4","5","6","7","8","9","0"};
const char *ROW2[] = {"q","w","e","r","t","y","u","i","o","p"};
const char *ROW3[] = {"a","s","d","f","g","h","j","k","l"};
const char *ROW4[] = {"z","x","c","v","b","n","m",",",".","?"};

} // namespace

void Keyboard::layout(double w, double h) {
    w_ = w; h_ = h;
    double kh    = 64.0;
    double rows  = 5;
    double total_h = rows * kh + 8;
    top_y_ = h - total_h;
    keys_.clear();

    auto add_row = [&](const char **labels, int n, double y) {
        double margin = 8.0;
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
    add_row(ROW1, 10, top_y_ + 4);
    add_row(ROW2, 10, top_y_ + 4 + kh);
    add_row(ROW3, 9,  top_y_ + 4 + 2 * kh);
    add_row(ROW4, 10, top_y_ + 4 + 3 * kh);

    // Last row: Shift, Space, Backspace, Enter
    double y = top_y_ + 4 + 4 * kh;
    double margin = 8.0;
    double remain = w_ - 2 * margin;
    Key shift{{margin, y, remain * 0.15, kh - 6}, "Shift", "Shift"};
    Key space{{margin + remain * 0.18, y, remain * 0.40, kh - 6}, "Space", " "};
    Key bksp {{margin + remain * 0.60, y, remain * 0.18, kh - 6}, "Bksp", "Backspace"};
    Key entr {{margin + remain * 0.80, y, remain * 0.20, kh - 6}, "Enter", "Enter"};
    keys_.push_back(shift);
    keys_.push_back(space);
    keys_.push_back(bksp);
    keys_.push_back(entr);
}

void Keyboard::draw(cairo_t *cr) {
    if (!visible_) return;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);
    cairo_rectangle(cr, 0, top_y_, w_, h_ - top_y_); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, top_y_); cairo_line_to(cr, w_, top_y_);
    cairo_stroke(cr);

    PangoFontDescription *fd = pango_font_description_from_string("Sans 18");
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
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i].r.contains(x, y)) {
            pressed_idx_ = (int)i;
            return true;
        }
    }
    return true;  // consume taps in the keyboard band
}

bool Keyboard::release(double x, double y) {
    if (!visible_ || pressed_idx_ < 0) return false;
    if (!keys_[pressed_idx_].r.contains(x, y)) {
        pressed_idx_ = -1;
        return true;
    }
    const Key &k = keys_[pressed_idx_];
    pressed_idx_ = -1;

    if (k.output == "Shift") {
        shift_ = !shift_;
    } else if (k.output == "Backspace" || k.output == "Enter") {
        if (key_cb_) key_cb_(k.output);
    } else if (k.output == " ") {
        if (text_cb_) text_cb_(" ");
    } else {
        std::string out = k.output;
        if (shift_ && out.size() == 1 && out[0] >= 'a' && out[0] <= 'z') {
            out[0] = (char)(out[0] - 32);
            shift_ = false;
        }
        if (text_cb_) text_cb_(out);
    }
    return true;
}

} // namespace bn
