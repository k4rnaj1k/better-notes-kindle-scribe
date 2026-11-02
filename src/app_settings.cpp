// Settings (home folder, export folder, file-watch toggle, keyboard layout;
// Bluetooth keyboards open their own page, app_bluetooth.cpp), persisted to a
// small JSON file OUTSIDE the vault (the vault path is itself a setting), and
// the Syncthing file watch.

#include "app.h"

#include "canvas.h"
#include "json.h"
#include "ui.h"

#include <algorithm>
#include <cstdlib>

namespace bn {

namespace {

// File-watch poll interval. e-ink + battery: 3s is responsive enough for a
// background Syncthing update without churning wake-ups.
constexpr unsigned kWatchIntervalMs = 3000;

gboolean cb_watch_tick(gpointer self) {
    static_cast<App *>(self)->on_watch_tick();
    return TRUE;   // keep the timer running (G_SOURCE_CONTINUE)
}

constexpr double kSettingsRowH = 100.0;
constexpr int    kSettingsRows = 5;
constexpr double kSettingsTop  = 100.0;   // first row, below the title

} // namespace

// --------------------------------------------------------------- config

void App::load_config() {
    if (config_path_.empty()) {
        const char *e = std::getenv("BN_CONFIG");
        config_path_ = e ? e : "/mnt/us/betternotes.json";
    }
    std::string raw;
    if (!read_file(config_path_, raw)) return;   // first run: keep defaults
    json::Value v;
    if (!json::parse(raw, v) || v.type != json::Type::Object) return;
    std::string home = v.str("home_folder", "");
    if (!home.empty()) notes_dir_ = home;
    export_dir_ = v.str("export_folder", export_dir_);
    if (auto *w = v.get("watch_files"); w && w->type == json::Type::Bool)
        watch_enabled_ = w->b;
    layout_ = layout_from_id(v.str("keyboard_layout", layout_id(layout_)));
}

void App::save_config() const {
    json::Value o = json::Value::make_obj();
    o.obj.emplace("home_folder",   json::Value::make_str(notes_dir_));
    o.obj.emplace("export_folder", json::Value::make_str(export_dir_));
    o.obj.emplace("watch_files",   json::Value::make_bool(watch_enabled_));
    o.obj.emplace("keyboard_layout", json::Value::make_str(layout_id(layout_)));
    write_file(config_path_, json::serialize(o));
}

void App::set_home_folder(const std::string &path) {
    if (path.empty() || path == notes_dir_) return;
    // Flush outstanding work into the OLD vault before re-rooting.
    if (note_.dirty) save_current();
    flush_markdown();
    notes_dir_ = path;
    ensure_dir(notes_dir_);
    // Drop open-document state so nothing saves back into the old vault.
    note_ = Note{};
    clear_page_cache();
    markdown_path_.clear();
    md_.clear();
    markdown_dirty_ = false;
    browser_path_.clear();
    browser_scroll_ = 0.0;
    canvas_set_vault_root(notes_dir_);
    index_.open(notes_dir_, "");
    browser_.set_current_path("");
    browser_sig_ = browser_signature();
    save_config();
    status_ = "home → " + notes_dir_;
}

// ------------------------------------------------------- settings modal

Rect App::settings_card_rect() const {
    double cw = std::min(720.0, (double)win_w_ * 0.9);
    double ch = kSettingsTop + kSettingsRows * kSettingsRowH + 112.0;
    return Rect{((double)win_w_ - cw) / 2.0, 140.0, cw, ch};
}

Rect App::settings_row_btn_rect(int row) const {
    // Rows 0-1: an Edit button; row 2: a switch; row 3: the EN/UA control;
    // row 4: Bluetooth's Manage button.
    Rect c = settings_card_rect();
    double bw = row == 2 ? 96.0 : row == 3 ? 200.0 : 150.0;
    double bh = row == 2 ? 52.0 : 56.0;
    double top = c.y + kSettingsTop + row * kSettingsRowH;
    return Rect{c.x + c.w - 28.0 - bw, top + (kSettingsRowH - bh) / 2.0, bw, bh};
}

Rect App::settings_close_btn_rect() const {
    Rect c = settings_card_rect();
    double bw = 170.0, bh = 60.0;
    return Rect{c.x + c.w - 28.0 - bw, c.y + c.h - 28.0 - bh, bw, bh};
}

void App::open_settings() {
    settings_open_ = true;
    status_ = "settings";
    redraw();
}

void App::close_settings() {
    settings_open_ = false;
    redraw();
}

void App::draw_settings_modal(cairo_t *cr, int win_w, int win_h) {
    using namespace theme;
    ui_backdrop(cr, win_w, win_h);
    Rect c = settings_card_rect();
    ui_card(cr, c);
    ui_text(cr, c.x + 28, c.y + 26, "Settings", kFontTitle);

    // One row = a label, a value underneath (ellipsized at the start so the
    // end of a path stays readable), and a control on the right.
    auto row = [&](int i, const char *label, const std::string &value) {
        double top = c.y + kSettingsTop + i * kSettingsRowH;
        Rect br = settings_row_btn_rect(i);
        ui_hline(cr, c.x + 28, c.x + c.w - 28, top);
        ui_text(cr, c.x + 28, top + 20, label, "Sans Bold 18");
        ui_text_ellipsized(cr, c.x + 28, top + 54, br.x - (c.x + 28) - 20,
                           value, kFontSmall, kMuted, true);
    };

    row(0, "Home folder", notes_dir_);
    ui_button(cr, settings_row_btn_rect(0), "Edit", Btn::Secondary);
    row(1, "Export folder",
        export_dir_.empty() ? "beside each note" : export_dir_);
    ui_button(cr, settings_row_btn_rect(1), "Edit", Btn::Secondary);
    row(2, "Watch files",
        watch_enabled_ ? "reloads when Syncthing changes a file" : "off");
    ui_switch(cr, settings_row_btn_rect(2), watch_enabled_);
    row(3, "Keyboard layout", "Ctrl+Space or Alt+Shift switches");
    static const char *const kLayouts[] = {"EN", "UA"};
    ui_segmented(cr, settings_row_btn_rect(3), kLayouts, 2,
                 layout_ == Layout::UA ? 1 : 0);
    int kbds = hid_.device_count();
    row(4, "Bluetooth keyboards",
        kbds == 0 ? "pair, connect or forget a keyboard"
      : kbds == 1 ? "1 keyboard connected"
                  : std::to_string(kbds) + " keyboards connected");
    ui_button(cr, settings_row_btn_rect(4), "Manage", Btn::Secondary);
    ui_hline(cr, c.x + 28, c.x + c.w - 28,
             c.y + kSettingsTop + kSettingsRows * kSettingsRowH);

    ui_button(cr, settings_close_btn_rect(), "Done", Btn::Primary);
}

void App::handle_settings_release(double x, double y) {
    if (settings_close_btn_rect().contains(x, y)) { close_settings(); return; }

    if (settings_row_btn_rect(0).contains(x, y)) {
        open_input(InputPurpose::HomeFolder, "Home folder (full path)", notes_dir_);
        return;
    }
    if (settings_row_btn_rect(1).contains(x, y)) {
        open_input(InputPurpose::ExportFolder,
                   "Export folder (blank = beside note)", export_dir_);
        return;
    }
    if (settings_row_btn_rect(2).contains(x, y)) {        // toggle file-watch
        watch_enabled_ = !watch_enabled_;
        if (watch_enabled_) start_file_watch(); else stop_file_watch();
        save_config();
        redraw();
        return;
    }
    if (settings_row_btn_rect(3).contains(x, y)) {        // EN | UA
        Rect r = settings_row_btn_rect(3);
        set_kbd_layout(x < r.x + r.w / 2.0 ? Layout::US : Layout::UA);
        return;
    }
    if (settings_row_btn_rect(4).contains(x, y)) { open_bluetooth(); return; }
    // Tap outside the card closes it.
    if (!settings_card_rect().contains(x, y)) close_settings();
}

// ----------------------------------------------------------- file watch

void App::start_file_watch() {
    if (watch_source_) return;
    watch_source_ = g_timeout_add(kWatchIntervalMs, cb_watch_tick, this);
}

void App::stop_file_watch() {
    if (watch_source_) { g_source_remove(watch_source_); watch_source_ = 0; }
}

std::string App::browser_signature() const {
    std::string sig;
    sig.reserve(index_.entries().size() * 24);
    for (auto &e : index_.entries()) {
        sig += e.id;
        sig += ':';
        sig += std::to_string(e.updated_ms);
        sig += ';';
    }
    return sig;
}

void App::on_watch_tick() {
    if (!watch_enabled_) return;
    // Don't disturb an in-flight export, active drawing, text entry, or an
    // open settings modal (it would repaint underneath it).
    if (exporting_pdf_ || pen_down_ || draw_pen_down_ ||
        input_open_ || settings_open_) return;

    if (screen_ == Screen::Markdown && !markdown_path_.empty()) {
        uint32_t m = file_mtime_ms(markdown_path_);
        if (m != 0 && m > md_disk_mtime_) {
            // External update — overwrite the buffer with the on-disk version.
            std::string fresh;
            if (read_file(markdown_path_, fresh)) {
                md_.text = std::move(fresh);
                md_.clamp();
                markdown_history_.clear();   // snapshots predate the reload
                markdown_dirty_ = false;
                md_disk_mtime_  = m;
                md_lines_.clear();
                status_ = "reloaded (sync)";
                redraw_markdown_body();   // partial: text band only
                redraw_toolbar();         // refresh the status line
            }
        }
        return;
    }

    if (screen_ == Screen::Browser) {
        // Re-scan; redraw only if the listing actually changed so a quiet
        // vault never fires a (slow) full-screen e-ink refresh.
        index_.open(notes_dir_, browser_path_);
        std::string sig = browser_signature();
        if (sig != browser_sig_) {
            browser_sig_ = sig;
            int cells = browser_.cell_count(index_);
            if (browser_.focus() >= cells) browser_.set_focus(cells - 1);
            redraw();
        }
        return;
    }
    // NoteView is intentionally not auto-reloaded (multi-file, edit-in-progress
    // risk); see the settings note.
}

} // namespace bn
