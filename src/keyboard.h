#pragma once
// On-screen Cairo keyboard. Emits the same KeyEvent a hardware keyboard does
// (with hardware = false), so App::handle_key is the only place that knows
// what a key means. Letter layer follows the active Layout (EN / UA); the
// globe key emits Key::SwitchLayout and App decides.

#include "keys.h"
#include "util.h"

#include <cairo/cairo.h>
#include <functional>
#include <string>
#include <vector>

namespace bn {

class Keyboard {
public:
    using KeyCallback = std::function<void(const KeyEvent &)>;

    void layout(double window_w, double window_h);
    void draw(cairo_t *cr);

    // Returns true if (x, y) lands on the keyboard region (and a key was
    // dispatched). Suppresses pen-draw passthrough above keyboard rows.
    bool press(double x, double y);
    bool release(double x, double y);

    bool visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    void   set_layout(Layout l) { lang_ = l; }
    Layout lang() const { return lang_; }

    double top_y() const { return top_y_; }

    // Rect of the key touched by the last press/release (zero-size if the tap
    // missed every key). Lets the app repaint just that key for snappy,
    // flash-free feedback instead of refreshing the whole keyboard band.
    Rect last_key_rect() const { return last_key_rect_; }
    // True when the last release changed every key's label (Shift case flip or
    // Mode switch), so the caller must repaint the whole keyboard band.
    bool last_full_redraw() const { return last_full_redraw_; }

    void on_key(KeyCallback cb) { key_cb_ = std::move(cb); }

private:
    enum class Mode { Letters, Symbols };
    enum class Kind { Emit, ModeSwitch, Shift };
    enum class Look { Char, Special, Format, Accent };   // key styling

    struct Cap {
        Rect        r;
        std::string label;
        Kind        kind = Kind::Emit;
        Look        look = Look::Char;
        KeyEvent    out;
    };
    using Row = std::vector<std::pair<const char *, KeyEvent>>;
    void add_row(const Row &row, double y, Look look);
    void draw_cap(cairo_t *cr, const Cap &k, bool down) const;

    std::vector<Cap> keys_;
    bool   visible_ = false;
    bool   shift_   = false;
    Mode   mode_    = Mode::Letters;
    Layout lang_    = Layout::US;
    double top_y_   = 0;
    double w_ = 0, h_ = 0;
    double kh_ = 80.0;
    int    pressed_idx_ = -1;
    Rect   last_key_rect_{};
    bool   last_full_redraw_ = false;
    KeyCallback key_cb_;
};

} // namespace bn
