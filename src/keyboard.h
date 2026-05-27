#pragma once
#include "util.h"

#include <cairo/cairo.h>
#include <functional>
#include <string>

namespace bn {

class Keyboard {
public:
    using TextCallback = std::function<void(const std::string &)>;
    using KeyCallback  = std::function<void(const std::string &)>;  // "Backspace", "Enter", "Space", "Shift"

    void layout(double window_w, double window_h);
    void draw(cairo_t *cr);

    // Returns true if (x, y) lands on the keyboard region (and a key was
    // dispatched). Suppresses pen-draw passthrough above keyboard rows.
    bool press(double x, double y);
    bool release(double x, double y);

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    double top_y() const { return top_y_; }

    void on_text(TextCallback cb) { text_cb_ = std::move(cb); }
    void on_key (KeyCallback  cb) { key_cb_  = std::move(cb); }

private:
    struct Key { Rect r; std::string label; std::string output; };
    std::vector<Key> keys_;
    bool   visible_ = false;
    bool   shift_   = false;
    double top_y_   = 0;
    double w_ = 0, h_ = 0;
    int    pressed_idx_ = -1;
    TextCallback text_cb_;
    KeyCallback  key_cb_;
};

} // namespace bn
