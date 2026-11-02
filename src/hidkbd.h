#pragma once
// Hardware keyboard input straight from evdev.
//
// kindle-hid-passthrough (github.com/zampierilucas/kindle-hid-passthrough)
// runs a userspace Bluetooth stack and hands each paired keyboard to the
// kernel through /dev/uhid, so it shows up as an ordinary /dev/input/eventN
// node. The Kindle's X server doesn't reliably adopt hot-plugged keyboards,
// so, like the pen, we read the node ourselves. USB OTG keyboards work the same
// way.
//
// Everything runs on the GLib main loop (GIOChannel watches), not a thread:
// key events are rare enough that a thread + queue would only add locking.
// Hot-plug: inotify on /dev/input triggers a debounced rescan, and a device
// that vanishes (Bluetooth drop) is closed when its read fails.
//
// Env knobs (see kual/betternotes/bin/launch.sh):
//   BN_KBD_EVDEV=0  don't read evdev at all; rely on X/GTK key events instead
//   BN_KBD_GRAB=0   don't EVIOCGRAB the device (by default we take it so the
//                   keys don't also leak into the X server / framework)

#include "keys.h"

#include <glib.h>

#include <functional>
#include <string>
#include <vector>

namespace bn {

class HidKeyboard {
public:
    using KeyCallback     = std::function<void(const KeyEvent &)>;
    // Fired when a keyboard appears (added=true) or goes away.
    using HotplugCallback = std::function<void(const std::string &name, bool added)>;

    void start(KeyCallback on_key, HotplugCallback on_hotplug);
    void stop();

    // Keyboards currently open. While > 0, X/GTK key events are ignored so a
    // key that reaches us both ways is only handled once.
    int  device_count() const { return (int)devices_.size(); }

    // Layout used to turn key codes into text. Ctrl/Alt shortcuts always
    // use the US position of the key.
    void   set_layout(Layout l) { layout_ = l; }
    Layout layout() const { return layout_; }

private:
    struct Device {
        std::string path;
        std::string name;
        int         fd    = -1;
        guint       watch = 0;
    };

    void scan();
    void try_open(const std::string &path);
    void close_device(size_t i, bool notify);
    void on_readable(int fd);
    void on_inotify();
    void schedule_rescan(unsigned delay_ms);
    void handle_key(unsigned code, int value);
    void set_caps_led(bool on);

    static gboolean cb_device(GIOChannel *, GIOCondition, gpointer self);
    static gboolean cb_inotify(GIOChannel *, GIOCondition, gpointer self);
    static gboolean cb_rescan(gpointer self);

    std::vector<Device> devices_;
    KeyCallback     key_cb_;
    HotplugCallback hotplug_cb_;
    bool  running_    = false;
    bool  grab_       = true;
    int   inotify_fd_ = -1;
    guint inotify_watch_ = 0;
    guint rescan_src_ = 0;     // pending one-shot (debounced) or periodic rescan
    bool  periodic_   = false; // inotify unavailable → poll instead
    Layout layout_    = Layout::US;

    // Modifier state across all keyboards.
    bool shift_l_ = false, shift_r_ = false;
    bool ctrl_l_  = false, ctrl_r_  = false;
    bool alt_l_   = false, alt_r_   = false;   // alt_r_ doubles as AltGr
    bool caps_    = false;
};

} // namespace bn
