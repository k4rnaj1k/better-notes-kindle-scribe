#include "hidkbd.h"

#include "util.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace bn {

namespace {

constexpr const char *kInputDir = "/dev/input";
// udev creates the node, then fixes its permissions a moment later; waiting a
// little before probing avoids an EACCES on the first try.
constexpr unsigned kHotplugDelayMs  = 400;
constexpr unsigned kPollIntervalMs  = 5000;

bool test_bit(const unsigned long *bits, unsigned bit) {
    const unsigned per = 8 * sizeof(unsigned long);
    return (bits[bit / per] >> (bit % per)) & 1UL;
}

// Same test kindle-hid-passthrough's udev helper (dev_is_keyboard.sh) uses to
// tag a keyboard: it can type a Q. Enter too, so a gamepad or page turner
// that happens to report a stray letter isn't taken for one.
bool is_keyboard(int fd) {
    const unsigned per = 8 * sizeof(unsigned long);
    unsigned long bits[(KEY_MAX + per) / per];
    std::memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return test_bit(bits, KEY_Q) && test_bit(bits, KEY_ENTER);
}

// --- layouts ---------------------------------------------------------------
// One row per evdev code: unshifted / shifted output, an optional AltGr
// (third-level) character, and whether Caps Lock applies. A layout only lists
// the keys it changes; everything else falls back to US.
struct KeyMap {
    unsigned short code;
    const char *lo;
    const char *hi;
    const char *altgr;      // AltGr level, nullptr = none
    const char *altgr_hi;   // AltGr+Shift
    bool        letter;     // Caps Lock flips case
};

#define L(code, lo, hi)                {code, lo, hi, nullptr, nullptr, true}
#define LA(code, lo, hi, alt, alt_hi)  {code, lo, hi, alt, alt_hi, true}
#define P(code, lo, hi)                {code, lo, hi, nullptr, nullptr, false}

const KeyMap kUsLayout[] = {
    P(KEY_1, "1", "!"), P(KEY_2, "2", "@"), P(KEY_3, "3", "#"), P(KEY_4, "4", "$"),
    P(KEY_5, "5", "%"), P(KEY_6, "6", "^"), P(KEY_7, "7", "&"), P(KEY_8, "8", "*"),
    P(KEY_9, "9", "("), P(KEY_0, "0", ")"),
    P(KEY_MINUS, "-", "_"), P(KEY_EQUAL, "=", "+"),
    L(KEY_Q, "q", "Q"), L(KEY_W, "w", "W"), L(KEY_E, "e", "E"), L(KEY_R, "r", "R"),
    L(KEY_T, "t", "T"), L(KEY_Y, "y", "Y"), L(KEY_U, "u", "U"), L(KEY_I, "i", "I"),
    L(KEY_O, "o", "O"), L(KEY_P, "p", "P"),
    P(KEY_LEFTBRACE, "[", "{"), P(KEY_RIGHTBRACE, "]", "}"),
    L(KEY_A, "a", "A"), L(KEY_S, "s", "S"), L(KEY_D, "d", "D"), L(KEY_F, "f", "F"),
    L(KEY_G, "g", "G"), L(KEY_H, "h", "H"), L(KEY_J, "j", "J"), L(KEY_K, "k", "K"),
    L(KEY_L, "l", "L"),
    P(KEY_SEMICOLON, ";", ":"), P(KEY_APOSTROPHE, "'", "\""), P(KEY_GRAVE, "`", "~"),
    P(KEY_BACKSLASH, "\\", "|"), P(KEY_102ND, "\\", "|"),
    L(KEY_Z, "z", "Z"), L(KEY_X, "x", "X"), L(KEY_C, "c", "C"), L(KEY_V, "v", "V"),
    L(KEY_B, "b", "B"), L(KEY_N, "n", "N"), L(KEY_M, "m", "M"),
    P(KEY_COMMA, ",", "<"), P(KEY_DOT, ".", ">"), P(KEY_SLASH, "/", "?"),
    P(KEY_SPACE, " ", " "),
    P(KEY_KP0, "0", "0"), P(KEY_KP1, "1", "1"), P(KEY_KP2, "2", "2"),
    P(KEY_KP3, "3", "3"), P(KEY_KP4, "4", "4"), P(KEY_KP5, "5", "5"),
    P(KEY_KP6, "6", "6"), P(KEY_KP7, "7", "7"), P(KEY_KP8, "8", "8"),
    P(KEY_KP9, "9", "9"), P(KEY_KPDOT, ".", "."), P(KEY_KPPLUS, "+", "+"),
    P(KEY_KPMINUS, "-", "-"), P(KEY_KPASTERISK, "*", "*"), P(KEY_KPSLASH, "/", "/"),
};

// Ukrainian ЙЦУКЕН, as in Windows "Ukrainian (Enhanced)" / xkb "ua": the
// apostrophe sits on the ` key (Shift = ₴), ґ on the backslash key and on
// AltGr+г, and the ISO key next to Left Shift keeps \ and /.
const KeyMap kUaLayout[] = {
    P(KEY_GRAVE, "'", "₴"),
    P(KEY_2, "2", "\""), P(KEY_3, "3", "№"), P(KEY_4, "4", ";"),
    P(KEY_6, "6", ":"),   P(KEY_7, "7", "?"),
    L(KEY_Q, "й", "Й"), L(KEY_W, "ц", "Ц"), L(KEY_E, "у", "У"), L(KEY_R, "к", "К"),
    L(KEY_T, "е", "Е"), L(KEY_Y, "н", "Н"), LA(KEY_U, "г", "Г", "ґ", "Ґ"),
    L(KEY_I, "ш", "Ш"), L(KEY_O, "щ", "Щ"), L(KEY_P, "з", "З"),
    L(KEY_LEFTBRACE, "х", "Х"), L(KEY_RIGHTBRACE, "ї", "Ї"),
    L(KEY_A, "ф", "Ф"), L(KEY_S, "і", "І"), L(KEY_D, "в", "В"), L(KEY_F, "а", "А"),
    L(KEY_G, "п", "П"), L(KEY_H, "р", "Р"), L(KEY_J, "о", "О"), L(KEY_K, "л", "Л"),
    L(KEY_L, "д", "Д"), L(KEY_SEMICOLON, "ж", "Ж"), L(KEY_APOSTROPHE, "є", "Є"),
    L(KEY_Z, "я", "Я"), L(KEY_X, "ч", "Ч"), L(KEY_C, "с", "С"), L(KEY_V, "м", "М"),
    L(KEY_B, "и", "И"), L(KEY_N, "т", "Т"), L(KEY_M, "ь", "Ь"),
    L(KEY_COMMA, "б", "Б"), L(KEY_DOT, "ю", "Ю"), P(KEY_SLASH, ".", ","),
    L(KEY_BACKSLASH, "ґ", "Ґ"), P(KEY_102ND, "\\", "/"),
};

#undef L
#undef LA
#undef P

template <size_t N>
const KeyMap *find(const KeyMap (&table)[N], unsigned code) {
    for (const auto &m : table)
        if (m.code == code) return &m;
    return nullptr;
}

const KeyMap *lookup(Layout layout, unsigned code) {
    if (layout == Layout::UA)
        if (const KeyMap *m = find(kUaLayout, code)) return m;
    return find(kUsLayout, code);
}

Key command_for(unsigned code) {
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: return Key::Enter;
    case KEY_BACKSPACE: return Key::Backspace;
    case KEY_DELETE:    return Key::Delete;
    case KEY_TAB:       return Key::Tab;
    case KEY_ESC:       return Key::Escape;
    case KEY_LEFT:      return Key::Left;
    case KEY_RIGHT:     return Key::Right;
    case KEY_UP:        return Key::Up;
    case KEY_DOWN:      return Key::Down;
    case KEY_HOME:      return Key::Home;
    case KEY_END:       return Key::End;
    case KEY_PAGEUP:    return Key::PageUp;
    case KEY_PAGEDOWN:  return Key::PageDown;
    case KEY_F2:        return Key::F2;
    default:            return Key::None;
    }
}

} // namespace

void HidKeyboard::start(KeyCallback on_key, HotplugCallback on_hotplug) {
    if (running_) return;
    if (const char *v = std::getenv("BN_KBD_EVDEV"); v && v[0] == '0') {
        log_info("kbd: evdev reader disabled (BN_KBD_EVDEV=0)");
        return;
    }
    if (const char *v = std::getenv("BN_KBD_GRAB")) grab_ = v[0] != '0';
    key_cb_     = std::move(on_key);
    hotplug_cb_ = std::move(on_hotplug);
    running_    = true;

    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ >= 0 &&
        inotify_add_watch(inotify_fd_, kInputDir, IN_CREATE | IN_ATTRIB) >= 0) {
        GIOChannel *ch = g_io_channel_unix_new(inotify_fd_);
        inotify_watch_ = g_io_add_watch(ch, G_IO_IN, cb_inotify, this);
        g_io_channel_unref(ch);
    } else {
        log_err("kbd: inotify on %s failed (%s); polling every %us",
                kInputDir, std::strerror(errno), kPollIntervalMs / 1000);
        if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
        periodic_   = true;
        rescan_src_ = g_timeout_add(kPollIntervalMs, cb_rescan, this);
    }
    scan();
}

void HidKeyboard::stop() {
    if (!running_) return;
    running_ = false;
    while (!devices_.empty()) close_device(devices_.size() - 1, false);
    if (inotify_watch_) { g_source_remove(inotify_watch_); inotify_watch_ = 0; }
    if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
    if (rescan_src_) { g_source_remove(rescan_src_); rescan_src_ = 0; }
}

void HidKeyboard::scan() {
    DIR *d = opendir(kInputDir);
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string(kInputDir) + "/" + e->d_name;
        bool known = false;
        for (auto &dev : devices_) if (dev.path == path) { known = true; break; }
        if (!known) try_open(path);
    }
    closedir(d);
}

void HidKeyboard::try_open(const std::string &path) {
    // Read-write so we can drive the Caps Lock LED; fall back to read-only.
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    if (!is_keyboard(fd)) { ::close(fd); return; }

    char name[128] = "keyboard";
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

    if (grab_ && ioctl(fd, EVIOCGRAB, 1) < 0) {
        // Someone else (e.g. kindle-button-mapper set to "grab") owns it; we
        // won't see its events until they let go.
        log_err("kbd: can't grab %s (%s): %s", path.c_str(), name,
                std::strerror(errno));
    }

    Device dev;
    dev.path = path;
    dev.name = name;
    dev.fd   = fd;
    GIOChannel *ch = g_io_channel_unix_new(fd);
    dev.watch = g_io_add_watch(ch, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                               cb_device, this);
    g_io_channel_unref(ch);
    devices_.push_back(dev);
    log_info("kbd: opened %s (%s)", path.c_str(), name);

    set_caps_led(caps_);
    if (hotplug_cb_) hotplug_cb_(dev.name, true);
}

void HidKeyboard::close_device(size_t i, bool notify) {
    Device dev = devices_[i];
    devices_.erase(devices_.begin() + i);
    if (dev.watch) g_source_remove(dev.watch);
    ::close(dev.fd);
    log_info("kbd: closed %s (%s)", dev.path.c_str(), dev.name.c_str());
    // Modifiers held on a keyboard that just dropped would otherwise stick.
    shift_l_ = shift_r_ = ctrl_l_ = ctrl_r_ = alt_l_ = alt_r_ = false;
    if (notify && hotplug_cb_) hotplug_cb_(dev.name, false);
}

gboolean HidKeyboard::cb_device(GIOChannel *ch, GIOCondition, gpointer self) {
    static_cast<HidKeyboard *>(self)->on_readable(g_io_channel_unix_get_fd(ch));
    // The watch is removed in close_device when the device goes away;
    // returning TRUE here keeps it otherwise.
    return TRUE;
}

void HidKeyboard::on_readable(int fd) {
    struct input_event evs[32];
    for (;;) {
        ssize_t n = ::read(fd, evs, sizeof(evs));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n <= 0) {
            // ENODEV: the Bluetooth link dropped and uhid removed the node.
            for (size_t i = 0; i < devices_.size(); ++i)
                if (devices_[i].fd == fd) { close_device(i, true); break; }
            return;
        }
        size_t count = (size_t)n / sizeof(evs[0]);
        for (size_t i = 0; i < count; ++i)
            if (evs[i].type == EV_KEY) handle_key(evs[i].code, evs[i].value);
    }
}

gboolean HidKeyboard::cb_inotify(GIOChannel *, GIOCondition, gpointer self) {
    static_cast<HidKeyboard *>(self)->on_inotify();
    return TRUE;
}

void HidKeyboard::on_inotify() {
    alignas(struct inotify_event) char buf[1024];
    bool relevant = false;
    for (;;) {
        ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0) break;
        for (char *p = buf; p < buf + n;) {
            auto *ev = reinterpret_cast<struct inotify_event *>(p);
            if (ev->len && std::strncmp(ev->name, "event", 5) == 0) relevant = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (relevant) schedule_rescan(kHotplugDelayMs);
}

void HidKeyboard::schedule_rescan(unsigned delay_ms) {
    if (periodic_) return;                 // the poll timer covers it
    if (rescan_src_) g_source_remove(rescan_src_);
    rescan_src_ = g_timeout_add(delay_ms, cb_rescan, this);
}

gboolean HidKeyboard::cb_rescan(gpointer self) {
    auto *k = static_cast<HidKeyboard *>(self);
    k->scan();
    if (k->periodic_) return TRUE;
    k->rescan_src_ = 0;
    return FALSE;   // one-shot (G_SOURCE_REMOVE; the Kindle's GLib predates it)
}

void HidKeyboard::set_caps_led(bool on) {
    struct input_event ev[2];
    std::memset(ev, 0, sizeof(ev));
    ev[0].type = EV_LED; ev[0].code = LED_CAPSL; ev[0].value = on ? 1 : 0;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    for (auto &d : devices_) {
        // Best effort: fails harmlessly on a read-only fd or LED-less keyboard.
        ssize_t r = ::write(d.fd, ev, sizeof(ev));
        (void)r;
    }
}

void HidKeyboard::handle_key(unsigned code, int value) {
    bool down = value != 0;   // 1 = press, 2 = autorepeat, 0 = release
    bool shift = shift_l_ || shift_r_;
    bool alt   = alt_l_ || alt_r_;
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
        // Alt+Shift switches layout (the usual Windows / Ukrainian habit).
        if (value == 1 && alt && key_cb_) key_cb_(key_cmd(Key::SwitchLayout, true));
        (code == KEY_LEFTSHIFT ? shift_l_ : shift_r_) = down;
        return;
    case KEY_LEFTALT: case KEY_RIGHTALT:
        if (value == 1 && shift && key_cb_) key_cb_(key_cmd(Key::SwitchLayout, true));
        (code == KEY_LEFTALT ? alt_l_ : alt_r_) = down;
        return;
    case KEY_LEFTCTRL:   ctrl_l_ = down; return;
    case KEY_RIGHTCTRL:  ctrl_r_ = down; return;
    case KEY_CAPSLOCK:
        if (value == 1) { caps_ = !caps_; set_caps_led(caps_); }
        return;
    }
    if (!down || !key_cb_) return;

    KeyEvent e;
    e.hardware = true;
    bool ctrl = ctrl_l_ || ctrl_r_;
    if (shift) e.mods |= kModShift;
    if (ctrl)  e.mods |= kModCtrl;

    if (ctrl && code == KEY_SPACE) { key_cb_(key_cmd(Key::SwitchLayout, true)); return; }

    if ((e.key = command_for(code)) != Key::None) {
        if (alt) e.mods |= kModAlt;
        key_cb_(e);
        return;
    }

    const KeyMap *m = lookup(layout_, code);
    if (!m) return;
    e.key = Key::Text;
    // Right Alt is AltGr: it types the layout's third-level character (ґ on
    // the Ukrainian г key). Otherwise either Alt is a shortcut modifier.
    if (alt_r_ && !alt_l_ && !ctrl && m->altgr) {
        bool upper = shift != (caps_ && m->letter);
        e.text = upper && m->altgr_hi ? m->altgr_hi : m->altgr;
        key_cb_(e);
        return;
    }
    if (alt) e.mods |= kModAlt;
    if (ctrl || alt) {
        // Shortcuts match on the physical US key, so Ctrl+S is Ctrl+S in
        // every layout.
        const KeyMap *us = lookup(Layout::US, code);
        e.text = us ? us->lo : m->lo;
    } else {
        bool upper = shift != (caps_ && m->letter);
        e.text = upper ? m->hi : m->lo;
    }
    key_cb_(e);
}

} // namespace bn
