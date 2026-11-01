// Bluetooth keyboard page: a card over Settings that drives the
// kindle-hid-passthrough daemon (see btclient.h) to scan for, pair, connect,
// disconnect and forget keyboards. Pairing and connecting happen in the
// daemon; once a keyboard is connected it shows up as /dev/input/eventN and
// HidKeyboard picks it up like any other.

#include "app.h"

#include "ui.h"

#include <algorithm>

namespace bn {

namespace {

// Poll every second while something is happening, every third second when idle.
constexpr unsigned kTickMs   = 1000;
constexpr int      kIdleEvery = 3;

constexpr double kRowH     = 96.0;
constexpr double kBannerH  = 128.0;
constexpr double kSectionH = 44.0;
constexpr int    kMaxPaired = 4;

gboolean cb_bt_tick(gpointer self) {
    static_cast<App *>(self)->bt_tick();
    return TRUE;
}

bool plain(const KeyEvent &e, char c) {
    return e.key == Key::Text && !e.ctrl() && !e.alt() && e.text.size() == 1 &&
           g_ascii_tolower(e.text[0]) == c;
}

bool same_addr(const std::string &a, const std::string &b) {
    return g_ascii_strcasecmp(a.c_str(), b.c_str()) == 0;
}

std::string display_name(const BtDevice &d) {
    return d.name.empty() ? d.address : d.name;
}

const char *protocol_label(const BtDevice &d) {
    return d.protocol == "classic" ? "Classic" : "BLE";
}

} // namespace

// ------------------------------------------------------------ open / close

void App::open_bluetooth() {
    bt_.open = true;
    bt_.sel = 0;
    bt_.note.clear();
    bt_.forget_armed.clear();
    bt_.sig.clear();
    status_ = "bluetooth";
    if (!bt_.timer) bt_.timer = g_timeout_add(kTickMs, cb_bt_tick, this);
    bt_poll();
    redraw();
}

void App::close_bluetooth() {
    bt_.open = false;
    if (bt_.timer) { g_source_remove(bt_.timer); bt_.timer = 0; }
    redraw();   // back to Settings
}

// ------------------------------------------------------------------ polling

void App::bt_tick() {
    if (!bt_.open) return;
    bool busy = bt_.scan_wait || !bt_.pair_addr.empty() || bt_.starting ||
                bt_.st.scanning || bt_.st.pairing;
    if (busy || ++bt_.ticks % kIdleEvery == 0) bt_poll();
}

void App::bt_poll() {
    if (bt_.polling) return;
    bt_.polling = true;
    bt_request("/status", [this](const BtReply &r) {
        bt_.reached = r.reached;
        if (r.ok) {
            bt_.st = bt_parse_status(r.body);
            bt_.starting = false;
            // A device that just got paired drops out of the nearby list.
            bt_set_found(std::move(bt_.found));
        } else if (!r.reached) {
            bt_.st = BtStatus{};
        }
        bt_poll_scan();
    });
}

void App::bt_poll_scan() {
    if (!bt_.reached || !bt_.scan_wait) { bt_poll_pair(); return; }
    bt_request("/scan-status", [this](const BtReply &r) {
        if (r.reached) {
            auto *s = r.body.get("scanning");
            bool scanning = s && s->type == json::Type::Bool && s->b;
            if (r.ok) bt_set_found(bt_parse_devices(r.body));
            if (!r.ok) {
                bt_.scan_wait = false;
                bt_.note = "Scan failed: " + r.error;
            } else if (!scanning) {
                bt_.scan_wait = false;
                bt_.note = bt_.found.empty()
                    ? "Nothing found. Put the keyboard in pairing mode and scan again."
                    : "Scan finished. Tap Pair next to your keyboard.";
            }
        }
        bt_poll_pair();
    });
}

void App::bt_poll_pair() {
    if (!bt_.reached || bt_.pair_addr.empty()) { bt_poll_done(); return; }
    bt_request("/pair-status", [this](const BtReply &r) {
        auto *p = r.body.get("pairing");
        bool pairing = r.ok && p && p->type == json::Type::Bool && p->b;
        if (r.reached && !pairing) {
            if (r.ok) {
                bt_.note = "Paired " + bt_.pair_name +
                           ". It reconnects by itself from now on.";
                status_ = "paired " + bt_.pair_name;
            } else {
                bt_.note = "Couldn't pair " + bt_.pair_name + ": " + r.error +
                           ". Check it's still in pairing mode and try again.";
            }
            bt_.pair_addr.clear();
            bt_.pin.clear();
            bt_poll_done();
            return;
        }
        // Still pairing: look for a passkey the keyboard wants typed on it.
        bt_request("/logs?lines=40", [this](const BtReply &lr) {
            if (lr.ok && !bt_.pair_addr.empty()) {
                std::string pin = bt_find_pin(lr.body);
                if (!pin.empty() && pin != bt_.pin_before) bt_.pin = pin;
            }
            bt_poll_done();
        });
    });
}

void App::bt_poll_done() {
    bt_.polling = false;
    if (!bt_.open) return;
    auto rows = bt_layout().rows.size();
    bt_.sel = rows ? std::min(bt_.sel, (int)rows - 1) : 0;
    std::string sig = bt_signature();
    if (sig == bt_.sig) return;   // nothing visible changed: no e-ink refresh
    bt_.sig = sig;
    if (settings_open_ && !input_open_) redraw_rect(bt_layout().card);
}

void App::bt_set_found(std::vector<BtDevice> devs) {
    std::vector<BtDevice> out;
    for (auto &d : devs) {
        bool known = false;
        for (auto &p : bt_.st.paired) known |= same_addr(p.address, d.address);
        for (auto &o : out) {
            if (!same_addr(o.address, d.address)) continue;
            known = true;
            o = d;   // later reports carry fresher RSSI / names
        }
        if (!known) out.push_back(std::move(d));
    }
    // Named devices first (keyboards announce a name), strongest signal first.
    std::stable_sort(out.begin(), out.end(), [](const BtDevice &a, const BtDevice &b) {
        if (a.name.empty() != b.name.empty()) return !a.name.empty();
        return a.rssi > b.rssi;
    });
    bt_.found = std::move(out);
}

std::string App::bt_signature() const {
    std::string s;
    s += bt_.reached ? 'R' : '-';
    s += bt_.st.running ? 'O' : '-';
    s += bt_.st.scanning || bt_.scan_wait ? 'S' : '-';
    s += bt_.starting ? 'W' : '-';
    for (auto &d : bt_.st.paired)
        s += "|" + d.address + d.name + (d.connected ? "c" : "") + (d.ready ? "r" : "");
    s += "#";
    for (auto &d : bt_.found) s += "|" + d.address + d.name;
    s += "#" + bt_.pair_addr + "#" + bt_.pin + "#" + bt_.note + "#" + bt_.forget_armed;
    s += "#" + std::to_string(bt_.sel);
    return s;
}

// ------------------------------------------------------------------ actions

void App::bt_scan() {
    if (!bt_.reached || !bt_.st.running || bt_.scan_wait || !bt_.pair_addr.empty())
        return;
    bt_.scan_wait = true;
    bt_.found.clear();
    bt_.note.clear();
    bt_request("/scan", [this](const BtReply &r) {
        if (!r.ok) {
            bt_.scan_wait = false;
            bt_.note = "Couldn't scan: " + r.error;
        }
        bt_poll();
    });
    bt_.sig.clear();
    redraw_rect(bt_layout().card);
}

void App::bt_toggle_power() {
    if (!bt_.reached) return;
    bt_.note = bt_.st.running ? "Bluetooth turned off." : "";
    bt_request(bt_.st.running ? "/stop" : "/start", [this](const BtReply &) { bt_poll(); });
}

void App::bt_activate(int row) {
    BtLayout L = bt_layout();
    if (row < 0 || row >= (int)L.rows.size()) return;
    const BtRow &r = L.rows[row];
    bt_.forget_armed.clear();

    if (r.paired) {
        const BtDevice &d = bt_.st.paired[r.idx];
        std::string q = "?addr=" + bt_query_escape(d.address);
        if (d.connected) {
            bt_.note = "Disconnected " + display_name(d) + ".";
            bt_request("/disconnect" + q, [this](const BtReply &) { bt_poll(); });
        } else {
            bt_.note = "Connecting to " + display_name(d) +
                       "… press a key on it to wake it up.";
            bt_request("/connect" + q + "&protocol=" + bt_query_escape(d.protocol),
                       [this](const BtReply &) { bt_poll(); });
        }
    } else {
        if (!bt_.pair_addr.empty() || bt_.scan_wait) return;   // one thing at a time
        const BtDevice &d = bt_.found[r.idx];
        bt_.pair_addr = d.address;
        bt_.pair_name = display_name(d);
        bt_.pin.clear();
        bt_.note.clear();
        std::string q = "?addr=" + bt_query_escape(d.address) +
                        "&protocol=" + bt_query_escape(d.protocol);
        if (!d.name.empty()) q += "&name=" + bt_query_escape(d.name);
        // Remember any passkey already in the log so an old one isn't shown.
        bt_request("/logs?lines=40", [this, q](const BtReply &lr) {
            bt_.pin_before = lr.ok ? bt_find_pin(lr.body) : "";
            bt_request("/pair" + q, [this](const BtReply &r) {
                if (!r.ok) {
                    bt_.note = "Couldn't pair " + bt_.pair_name + ": " + r.error;
                    bt_.pair_addr.clear();
                }
                bt_poll();
            });
        });
    }
    bt_.sig.clear();
    redraw_rect(L.card);
}

void App::bt_forget(int row) {
    BtLayout L = bt_layout();
    if (row < 0 || row >= (int)L.rows.size() || !L.rows[row].paired) return;
    const BtDevice &d = bt_.st.paired[L.rows[row].idx];
    if (!same_addr(bt_.forget_armed, d.address)) {
        bt_.forget_armed = d.address;   // ask once more before removing keys
    } else {
        bt_.forget_armed.clear();
        bt_.note = "Forgot " + display_name(d) + ". Pair it again to use it.";
        bt_request("/remove?addr=" + bt_query_escape(d.address),
                   [this](const BtReply &r) {
                       if (!r.ok) bt_.note = "Couldn't forget it: " + r.error;
                       bt_poll();
                   });
    }
    bt_.sig.clear();
    redraw_rect(L.card);
}

// ------------------------------------------------------------------- layout

BtLayout App::bt_layout() const {
    BtLayout L;
    double cw = std::min(860.0, (double)win_w_ * 0.92);
    double ch = std::min(1480.0, (double)win_h_ - 240.0);
    L.card = Rect{((double)win_w_ - cw) / 2.0, 120.0, cw, ch};
    const Rect &c = L.card;
    double x0 = c.x + 28, inner = c.w - 56;

    L.power   = Rect{c.x + c.w - 28 - 96, c.y + 34, 96, 52};
    L.done    = Rect{c.x + c.w - 28 - 170, c.y + c.h - 28 - 60, 170, 60};
    L.banner  = Rect{x0, L.done.y - 24 - kBannerH, inner, kBannerH};
    L.service = Rect{c.x + (c.w - 260) / 2.0, c.y + 360, 260, 64};

    if (!bt_.reached || !bt_.st.running) return L;

    double y = c.y + 124;
    L.paired_top = y;
    y += kSectionH;
    int np = std::min((int)bt_.st.paired.size(), kMaxPaired);
    for (int i = 0; i < np; ++i, y += kRowH) {
        BtRow r;
        r.row    = Rect{x0, y, inner, kRowH};
        r.forget = Rect{x0 + inner - 8 - 64, y + 18, 64, 60};
        r.action = Rect{r.forget.x - 12 - 180, y + 18, 180, 60};
        r.paired = true;
        r.idx    = i;
        L.rows.push_back(r);
    }
    if (np == 0) y += 64;   // room for the "none yet" line

    y += 28;
    L.nearby_top = y;
    L.scan = Rect{x0 + inner - 190, y - 8, 190, 56};
    y += kSectionH + 16;
    double bottom = L.banner.y - 12;
    for (int i = 0; i < (int)bt_.found.size() && y + kRowH <= bottom; ++i, y += kRowH) {
        BtRow r;
        r.row    = Rect{x0, y, inner, kRowH};
        r.action = Rect{x0 + inner - 8 - 180, y + 18, 180, 60};
        r.paired = false;
        r.idx    = i;
        L.rows.push_back(r);
    }
    return L;
}

// --------------------------------------------------------------------- draw

void App::draw_bluetooth(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ui_backdrop(cr, win_w, win_h);
    BtLayout L = bt_layout();
    const Rect &c = L.card;
    double x0 = c.x + 28, inner = c.w - 56;
    ui_card(cr, c);

    ui_grey(cr, kInk);
    ui_icon(cr, Icon::Bluetooth, x0 + 14, c.y + 46, 18);
    ui_text(cr, x0 + 44, c.y + 26, "Bluetooth keyboards", kFontTitle);

    // Subtitle: the state of the radio and the connected keyboards.
    std::string sub;
    if (!bt_.reached) {
        sub = bt_.starting ? "starting the Bluetooth service…" : "service not running";
    } else if (!bt_.st.running) {
        sub = "Bluetooth is off";
    } else {
        int conn = 0;
        for (auto &d : bt_.st.paired) conn += d.ready;
        sub = conn == 0 ? "on · no keyboard connected"
            : conn == 1 ? "on · 1 keyboard connected"
                        : "on · " + std::to_string(conn) + " keyboards connected";
    }
    ui_text(cr, x0 + 44, c.y + 70, sub, kFontSmall, kMuted);
    if (bt_.reached) ui_switch(cr, L.power, bt_.st.running);
    ui_hline(cr, x0, x0 + inner, c.y + 108);

    // --- service missing / off -------------------------------------------
    if (!bt_.reached || !bt_.st.running) {
        std::string head, body;
        if (!bt_.reached && bt_installed()) {
            head = bt_.starting ? "Starting…" : "Bluetooth service isn't running";
            body = "BetterNotes uses kindle-hid-passthrough to talk to keyboards. "
                   "Start its service to pair or connect one.";
        } else if (!bt_.reached) {
            head = "kindle-hid-passthrough isn't installed";
            body = "Install it from github.com/zampierilucas/kindle-hid-passthrough "
                   "into " + bt_install_dir() + ", then come back here.";
        } else {
            head = "Bluetooth is off";
            body = "Turn it on with the switch above to pair or connect a keyboard.";
        }
        Rect headr{x0, c.y + 180, inner, 40};
        ui_text_centered(cr, headr, head, kFontHead);
        double bw = std::min(inner - 80, 620.0);
        ui_text_wrapped(cr, c.x + (c.w - bw) / 2.0, c.y + 240, bw, body, kFontBody, kMuted);
        if (!bt_.reached && bt_installed())
            ui_button(cr, L.service, bt_.starting ? "Starting…" : "Start service",
                      bt_.starting ? Btn::Secondary : Btn::Primary, kFontBody);
        ui_button(cr, L.done, "Done", Btn::Primary);
        return;
    }

    bool focus = hw_keyboard();
    auto section = [&](double y, const char *label) {
        ui_text(cr, x0, y + 6, label, "Sans Bold 14", kMuted);
    };
    auto row_bg = [&](int i, const BtRow &r) {
        if (focus && i == bt_.sel) {
            ui_fill_round(cr, r.row, kRadiusSm, kSurface);
            ui_focus_ring(cr, r.row, kRadiusSm);
        } else if (i > 0 && L.rows[i - 1].paired == r.paired) {
            ui_hline(cr, r.row.x + 16, r.row.x + r.row.w - 16, r.row.y);
        }
    };
    auto row_text = [&](const BtRow &r, Icon icon, const std::string &title,
                        const std::string &detail) {
        ui_grey(cr, kInk);
        ui_icon(cr, icon, r.row.x + 36, r.row.y + kRowH / 2, 18);
        double tx = r.row.x + 72, tw = r.action.x - 16 - tx;
        ui_text_ellipsized(cr, tx, r.row.y + 16, tw, title, "Sans Bold 18", kInk, false);
        ui_text_ellipsized(cr, tx, r.row.y + 54, tw, detail, kFontSmall, kMuted, false);
    };

    // --- paired ------------------------------------------------------------
    section(L.paired_top, "PAIRED");
    if (bt_.st.paired.empty()) {
        ui_text(cr, x0, L.paired_top + kSectionH + 14,
                "No keyboards paired yet.", kFontBody, kMuted);
    }
    for (int i = 0; i < (int)L.rows.size(); ++i) {
        const BtRow &r = L.rows[i];
        if (!r.paired) continue;
        const BtDevice &d = bt_.st.paired[r.idx];
        row_bg(i, r);
        bool armed = same_addr(bt_.forget_armed, d.address);
        std::string detail =
            armed         ? "Tap the bin again to forget it"
          : d.ready       ? std::string("Connected · ") + protocol_label(d)
          : d.connected   ? std::string("Connecting… · ") + protocol_label(d)
                          : std::string("Not connected · ") + protocol_label(d) +
                                " · " + d.address;
        row_text(r, Icon::Keyboard, display_name(d), detail);
        ui_button(cr, r.action, d.connected ? "Disconnect" : "Connect",
                  d.connected ? Btn::Secondary : Btn::Primary);
        ui_icon_button(cr, r.forget, Icon::Trash, "",
                       armed ? Btn::Primary : Btn::Ghost);
    }

    // --- nearby ------------------------------------------------------------
    bool scanning = bt_.scan_wait || bt_.st.scanning;
    section(L.nearby_top, "NEARBY");
    ui_button(cr, L.scan, scanning ? "Scanning…" : "Scan",
              Btn::Secondary, kFontLabel, scanning);
    if (bt_.found.empty()) {
        ui_text(cr, x0, L.nearby_top + kSectionH + 30,
                scanning ? "Looking for keyboards…" : "Tap Scan to look for keyboards.",
                kFontBody, kMuted);
    }
    for (int i = 0; i < (int)L.rows.size(); ++i) {
        const BtRow &r = L.rows[i];
        if (r.paired) continue;
        const BtDevice &d = bt_.found[r.idx];
        row_bg(i, r);
        bool this_one = same_addr(bt_.pair_addr, d.address);
        std::string detail = std::string(protocol_label(d)) + " · " + d.address;
        if (d.rssi) detail += " · " + std::to_string(d.rssi) + " dBm";
        row_text(r, Icon::Bluetooth, display_name(d), detail);
        ui_button(cr, r.action, this_one ? "Pairing…" : "Pair",
                  this_one ? Btn::Secondary : Btn::Primary, kFontLabel, this_one);
    }

    // --- banner: what's happening / what to do -------------------------------
    ui_fill_round(cr, L.banner, kRadiusSm, kSurface);
    Rect b{L.banner.x + 20, L.banner.y + 16, L.banner.w - 40, L.banner.h - 32};
    if (!bt_.pair_addr.empty()) {
        ui_text_ellipsized(cr, b.x, b.y, b.w, "Pairing with " + bt_.pair_name + "…",
                           "Sans Bold 18", kInk, false);
        if (!bt_.pin.empty()) {
            ui_text(cr, b.x, b.y + 34, "Type this on the keyboard, then press Enter:",
                    kFontLabel, kMuted);
            ui_text(cr, b.x + b.w - 200, b.y + 26, bt_.pin, "Sans Bold 34");
        } else {
            ui_text_wrapped(cr, b.x, b.y + 34, b.w,
                            "Keep the keyboard in pairing mode. This takes up to 30 s.",
                            kFontLabel, kMuted);
        }
    } else if (scanning) {
        ui_text(cr, b.x, b.y, "Scanning for 10 seconds…", "Sans Bold 18");
        ui_text_wrapped(cr, b.x, b.y + 34, b.w,
                        "Put the keyboard in pairing mode, usually by holding its "
                        "Bluetooth or Fn+1 key until the light blinks.",
                        kFontLabel, kMuted);
    } else if (!bt_.note.empty()) {
        ui_text_wrapped(cr, b.x, b.y, b.w, bt_.note, kFontBody);
    } else {
        ui_text_wrapped(cr, b.x, b.y, b.w,
                        "Paired keyboards reconnect by themselves while BetterNotes "
                        "is open. Press a key to wake a sleeping one.",
                        kFontLabel, kMuted);
    }

    if (focus) {
        ui_text(cr, x0, L.done.y + 18,
                "Up/Down choose · Enter connect/pair · Del forget · S scan",
                kFontSmall, kMuted);
    }
    ui_button(cr, L.done, "Done", Btn::Primary);
}

// -------------------------------------------------------------------- input

void App::handle_bluetooth_release(double x, double y) {
    BtLayout L = bt_layout();
    if (L.done.contains(x, y)) { close_bluetooth(); return; }
    if (!L.card.contains(x, y)) { close_bluetooth(); return; }

    if (!bt_.reached) {
        if (L.service.contains(x, y) && bt_installed() && !bt_.starting) {
            bt_.starting = true;
            bt_start_service();
            bt_.sig.clear();
            redraw_rect(L.card);
        }
        return;
    }
    if (L.power.contains(x, y)) { bt_toggle_power(); return; }
    if (!bt_.st.running) return;
    if (L.scan.contains(x, y)) { bt_scan(); return; }
    for (int i = 0; i < (int)L.rows.size(); ++i) {
        const BtRow &r = L.rows[i];
        if (r.action.contains(x, y)) { bt_.sel = i; bt_activate(i); return; }
        if (r.paired && r.forget.contains(x, y)) { bt_.sel = i; bt_forget(i); return; }
    }
    // A tap anywhere else disarms a pending Forget.
    if (!bt_.forget_armed.empty()) {
        bt_.forget_armed.clear();
        bt_.sig.clear();
        redraw_rect(L.card);
    }
}

void App::key_bluetooth(const KeyEvent &e) {
    if (e.key == Key::Escape) { close_bluetooth(); return; }
    if (!bt_.reached) {
        if (e.key == Key::Enter && bt_installed() && !bt_.starting) {
            bt_.starting = true;
            bt_start_service();
            bt_.sig.clear();
            redraw_rect(bt_layout().card);
        }
        return;
    }
    if (plain(e, 'b')) { bt_toggle_power(); return; }
    if (!bt_.st.running) return;
    if (plain(e, 's')) { bt_scan(); return; }

    int n = (int)bt_layout().rows.size();
    int sel = bt_.sel;
    switch (e.key) {
    case Key::Up:   sel = std::max(0, sel - 1); break;
    case Key::Down: sel = std::min(n - 1, sel + 1); break;
    case Key::Home: sel = 0; break;
    case Key::End:  sel = n - 1; break;
    case Key::Enter:     bt_activate(sel); return;
    case Key::Delete:
    case Key::Backspace: bt_forget(sel);   return;
    default: return;
    }
    sel = std::max(0, sel);
    if (sel == bt_.sel) return;
    bt_.sel = sel;
    bt_.forget_armed.clear();
    bt_.sig = bt_signature();
    redraw_rect(bt_layout().card);
}

} // namespace bn
