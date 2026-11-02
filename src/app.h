#pragma once
// The application: screens, modals, input routing and rendering.
//
// App is one class, but its implementation is split by concern:
//   app.cpp           startup, GTK glue, rendering, screen routing, save/export
//   app_input.cpp     pointer (finger) input + toolbar actions
//   app_keys.cpp      keyboard input (on-screen, Bluetooth/evdev, X)
//   app_pen.cpp       stylus samples → strokes, eraser, lasso; OCR results
//   app_markdown.cpp  markdown editor
//   app_modals.cpp    input / confirm / tags / export / draw modals
//   app_pickers.cpp   link picker + template picker
//   app_settings.cpp  persisted settings + Syncthing file watch
//   app_bluetooth.cpp Bluetooth keyboard page (pair / connect / forget)

#include "btclient.h"
#include "filebrowser.h"
#include "hidkbd.h"
#include "index.h"
#include "inkfb.h"
#include "keyboard.h"
#include "keys.h"
#include "lasso.h"
#include "links.h"
#include "markdown.h"
#include "ocr.h"
#include "pen.h"
#include "strokes.h"
#include "textedit.h"
#include "toolbar.h"
#include "tools.h"

#include <gtk/gtk.h>

#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace bn {

enum class Screen { Browser, NoteView, Markdown };

// Modal that appears after a lasso closes; user picks the target note.
struct LinkPicker {
    bool   open = false;
    Rect   anchor;          // rect on the current page where the link will sit
    int    page = -1;
    int    scroll = 0;      // first visible entry
    int    sel = 0;         // keyboard-selected entry
    std::vector<IndexEntry> entries;  // snapshot of vault at picker open time
};

// Geometry of the Bluetooth page, shared by draw and hit-test.
struct BtRow {
    Rect row;
    Rect action;            // Connect / Disconnect / Pair
    Rect forget;            // paired rows only (w == 0 otherwise)
    bool paired = false;
    int  idx    = 0;        // into BtPage::st.paired or BtPage::found
};
struct BtLayout {
    Rect   card, power, scan, service, done, banner;
    double paired_top = 0, nearby_top = 0;
    std::vector<BtRow> rows;     // paired first, then nearby
};

class App {
public:
    static App &instance();

    void set_notes_dir(const std::string &d) { notes_dir_ = d; }
    void set_tessdata_dir(const std::string &d) { tessdata_dir_ = d; }

    int run(int argc, char *argv[]);

    // --- entry points for the GTK / GLib trampolines ---
    void on_draw(cairo_t *cr, int win_w, int win_h);
    bool on_button_press(double x, double y);
    bool on_button_release(double x, double y);
    bool on_motion(double x, double y);
    bool on_gtk_key(GdkEventKey *e);     // X keyboard path (see app_keys.cpp)
    void process_async_events();         // drains pen_queue_ + ocr_queue_
    void on_watch_tick();                // file-watch poll (browser + markdown)
    void bt_tick();                      // Bluetooth page poll of the daemon

    // X11 window coords → drawing-space coords (inverse of the cairo
    // rotation applied in on_draw).
    void screen_to_drawing(double sx, double sy, double &dx, double &dy) const;

    // Called from the pen_reader thread; just enqueues onto pen_queue_.
    void on_pen_sample(const PenSample &s);

private:
    App();

    // ---------------------------------------------------------------- app.cpp
    void enter_browser();
    void enter_folder(const std::string &rel_path);  // descends in vault
    void enter_parent_folder();                       // ".." navigation
    void enter_note(const std::string &id);
    void enter_markdown(const std::string &path);
    void go_back();                                   // toolbar Back / Esc
    void quit();                                      // flush + gtk_main_quit

    void save_current();
    void export_current_pdf();

    void draw_note_view(cairo_t *cr, int win_w, int win_h);
    void draw_markdown_view(cairo_t *cr, int win_w, int win_h);
    void draw_show_tab(cairo_t *cr);   // "show toolbar" tab when hidden
    Rect show_tab_rect() const;

    void redraw();
    void redraw_rect(double x, double y, double w, double h);
    void redraw_rect(const Rect &r) { redraw_rect(r.x, r.y, r.w, r.h); }
    void redraw_toolbar();          // fast partial repaint of the toolbar strip
    void redraw_pen_menu_region();  // toolbar strip + pen dropdown panel only
    // Keyboard feedback: repaint only the key that changed so taps don't flash
    // the whole on-screen keyboard (full band only on Shift/Mode switches).
    void redraw_kbd_after_press();
    void redraw_kbd_after_release();
    void redraw_keyboard_band();

    // Page render cache (NoteView). Rendered page bitmaps (template +
    // committed strokes) reused across redraws, so flipping back to a page or
    // any partial repaint is a cheap blit instead of re-rasterising every
    // stroke. The current page's bitmap is updated incrementally on
    // draw/erase; others stay read-only until revisited.
    cairo_surface_t *page_surface(int page);            // get-or-render bitmap
    void update_page_cache_region(int page, const Rect &r);  // incremental repaint
    void invalidate_page_cache(int page);               // drop one page's bitmap
    void clear_page_cache();                             // drop all bitmaps

    // Software rotation. The Kindle's X server doesn't support XRandR, so we
    // pre-rotate the cairo context and inverse-rotate input events.
    void set_rotation(int deg);
    int  draw_w() const { return (rotation_ == 90 || rotation_ == 270) ? xh_ : xw_; }
    int  draw_h() const { return (rotation_ == 90 || rotation_ == 270) ? xw_ : xh_; }

    // ---------------------------------------------------------- app_input.cpp
    bool press_modal(double x, double y);     // true if a modal ate the press
    bool release_modal(double x, double y);
    void release_browser(double x, double y);
    void release_note_canvas(double x, double y);
    void release_markdown_body(double x, double y);
    void release_toolbar(double x, double y);
    void release_pen_menu(double x, double y);
    void handle_toolbar_action(ToolbarAction a);
    void handle_browser_action(const BrowserHit &h);
    void follow_link_at(double x, double y);
    void flip_page(int delta);
    void clamp_browser_scroll();

    // ----------------------------------------------------------- app_keys.cpp
    // Single sink for every key, whatever the source. Modals first, then the
    // active screen.
    void handle_key(const KeyEvent &e);
    bool key_global(const KeyEvent &e);
    bool key_input_modal(const KeyEvent &e);
    bool key_confirm_modal(const KeyEvent &e);
    bool key_draw_modal(const KeyEvent &e);
    bool key_browser(const KeyEvent &e);
    bool key_note(const KeyEvent &e);
    bool key_markdown(const KeyEvent &e);
    void on_hw_keyboard(const std::string &name, bool added);
    // Switch the text layout for both the Bluetooth and on-screen keyboards.
    void set_kbd_layout(Layout l);
    // A physical keyboard is in use: one we read through evdev, or one the X
    // server adopted itself (seen once it sends a key; X can't tell us when
    // it goes away). Hides the on-screen keyboard in modals and turns on the
    // focus rings.
    bool hw_keyboard() const { return hid_.device_count() > 0 || x_keys_seen_; }

    // ------------------------------------------------------------ app_pen.cpp
    void handle_note_sample(const PenSample &s);
    void handle_draw_sample(const PenSample &s);
    void handle_ocr_result(const OcrResult &r);
    void map_pen_to_page(const PenSample &s, double &px, double &py);
    Point pen_point(const PenSample &s, double px, double py) const;
    // Paint one live segment of `stroke` ending at p: through inkfb when
    // available (growing bbox), otherwise as a GTK partial redraw.
    void draw_live_segment(const Stroke &stroke, const Point &p, InkRect &bbox);

    // ------------------------------------------------------- app_markdown.cpp
    void markdown_snapshot();
    void markdown_undo();
    void markdown_insert(const std::string &text);
    void markdown_edit(const KeyEvent &e);   // TextEdit op + undo bookkeeping
    void markdown_newline();                 // Enter; continues lists / quotes
    void markdown_wrap(const std::string &marker);   // Ctrl+B / Ctrl+I / code
    void markdown_page(int dir);             // PageUp / PageDown
    void redraw_markdown_body();             // partial repaint of the text band
    double markdown_band_top() const;
    double markdown_band_bottom() const;
    void clamp_markdown_scroll();
    void markdown_set_cursor_from_tap(double x, double y);
    void markdown_move_cursor_vertical(int dir);   // -1 = up, +1 = down
    // Persist the markdown buffer (when dirty) and sync the filename to the H1
    // inline title — Obsidian-style: editing the title heading renames the
    // file. Call wherever we leave/switch a markdown file.
    void flush_markdown();
    // On open, make sure the file starts with an "# <filename>" heading so the
    // inline title is present and editable.
    void ensure_markdown_title();

    // --------------------------------------------------------- app_modals.cpp
    // Text input modal (rename, tags, new folder, settings paths). Its text
    // lives in input_; commit_input() dispatches on input_purpose_.
    enum class InputPurpose { Rename, Tags, NewFolder, HomeFolder, ExportFolder };
    void open_input(InputPurpose purpose, const std::string &title,
                    const std::string &initial,
                    const std::string &target_id = "");
    void commit_input();
    void close_input();
    void open_delete_confirm(const IndexEntry &e);
    void confirm_delete();
    void cancel_confirm();
    Rect modal_card_rect() const;             // shared centred card geometry
    Rect modal_btn_rect(bool primary) const;  // Save/Delete (primary) | Cancel
    Rect input_field_rect() const;
    void redraw_input_card();
    void draw_input_modal(cairo_t *cr);
    void draw_confirm_modal(cairo_t *cr, int win_w, int win_h);

    // Tags: per-page or notebook tags, edited through the input modal.
    enum class TagScope { Page, Notebook };
    void open_tags_editor();
    void commit_tags();                 // parse input_ into the active scope
    void load_tags_into_input();        // refill input_ from the active scope
    void toggle_tags_scope();
    Rect tags_scope_btn_rect() const;
    static std::vector<std::string> parse_tags(const std::string &csv);
    static std::string join_tags(const std::vector<std::string> &tags);

    // PDF export progress card.
    Rect export_overlay_rect() const;
    void draw_export_overlay(cairo_t *cr, int win_w, int win_h);

    // Draw modal (drawings / OCR), markdown only.
    enum class DrawPurpose { Image, Ocr };
    void open_draw_modal(DrawPurpose purpose);
    void close_draw_modal();
    void clear_draw_modal();
    void commit_draw_modal();
    Rect draw_canvas_rect() const;
    Rect draw_btn_rect(int which) const;   // 0=Save 1=Clear 2=Cancel
    void draw_draw_modal(cairo_t *cr, int win_w, int win_h);
    // Rasterise draw_strokes_ (cropped to their bbox + padding) to a PNG.
    // Returns false if there's nothing drawn or the write fails.
    bool draw_render_png(const std::string &abs_path);

    // -------------------------------------------------------- app_pickers.cpp
    void open_link_picker(const Rect &anchor);
    bool key_list_modal(const KeyEvent &e);   // keys for either picker
    void handle_picker_press(double x, double y);
    void pick_link(int idx);
    void draw_link_picker(cairo_t *cr, int win_w, int win_h);

    void open_template_picker();
    int  template_count() const { return 4 + (int)tmpl_custom_.size(); }
    void apply_template(TemplateId tmpl, const std::string &bg_image);
    void pick_template(int idx);
    void handle_template_picker_press(double x, double y);
    void draw_template_picker(cairo_t *cr, int win_w, int win_h);

    // ------------------------------------------------------- app_settings.cpp
    void load_config();                 // read config_path_ into the settings
    void save_config() const;           // persist the current settings
    void set_home_folder(const std::string &path);   // re-roots the vault
    void open_settings();
    void close_settings();
    void draw_settings_modal(cairo_t *cr, int win_w, int win_h);
    void handle_settings_release(double x, double y);
    Rect settings_card_rect() const;
    Rect settings_row_btn_rect(int row) const;   // edit/toggle button per row
    Rect settings_close_btn_rect() const;

    // ------------------------------------------------------ app_bluetooth.cpp
    void     open_bluetooth();
    void     close_bluetooth();
    BtLayout bt_layout() const;
    void     draw_bluetooth(cairo_t *cr, int win_w, int win_h);
    void     handle_bluetooth_release(double x, double y);
    void     key_bluetooth(const KeyEvent &e);
    void     bt_poll();                  // /status → /scan-status → /pair-status → /logs
    void     bt_poll_scan();
    void     bt_poll_pair();
    void     bt_poll_done();
    void     bt_set_found(std::vector<BtDevice> devs);
    void     bt_activate(int row);       // the row's main button
    void     bt_forget(int row);         // first call arms, second removes
    void     bt_scan();
    void     bt_toggle_power();
    std::string bt_signature() const;
    // Poll the open file / browser for external (Syncthing) updates and reload,
    // so background syncs show up without a restart.
    void start_file_watch();
    void stop_file_watch();
    std::string browser_signature() const;   // cheap "did the listing change?"

    // ================================================================ state
    GtkWidget    *window_  = nullptr;
    GtkWidget    *canvas_  = nullptr;
    int           xw_      = 0;     // raw X11 window size
    int           xh_      = 0;
    int           win_w_   = 0;     // post-rotation drawing-space size
    int           win_h_   = 0;
    // The Scribe's X server is portrait-native, so 0 keeps the UI portrait.
    // Override with BN_ROTATION.
    int           rotation_ = 0;

    Screen        screen_  = Screen::Browser;
    ToolState     tool_;
    Toolbar       toolbar_;
    FileBrowser   browser_;
    Keyboard      keyboard_;
    HidKeyboard   hid_;
    bool          x_keys_seen_ = false;   // X/GTK delivered a hardware key
    Layout        layout_      = Layout::US;   // persisted as keyboard_layout
    Lasso         lasso_;
    NavHistory    history_;

    NotesIndex    index_;
    std::string   browser_path_;     // vault-relative dir the browser is showing
    // Grid scroll offset (px) and the value captured at gesture start so a
    // finger drag scrolls relative to where it began.
    double        browser_scroll_   = 0.0;
    double        browser_scroll_at_press_ = 0.0;
    // Move-in-flight: set while the user is relocating an entry. They browse to
    // a destination folder and tap "Move here".
    bool          move_active_      = false;
    std::string   move_id_;          // entry id being moved
    std::string   move_title_;       // its display title (for the banner)
    Note          note_;             // currently-open note
    int           current_page_ = 0;

    // Page render cache. Keyed by (note id, drawing-space size); each entry is
    // a page index + its rendered bitmap. Small LRU (front = most recent).
    struct PageCache { int page; cairo_surface_t *surf; };
    std::vector<PageCache> page_cache_;
    std::string            page_cache_note_;
    int                    page_cache_w_ = 0, page_cache_h_ = 0;
    static constexpr int   kPageCacheMax = 3;

    // Markdown editor.
    std::string   markdown_path_;
    TextEdit      md_;
    bool          markdown_dirty_ = false;
    std::vector<std::pair<std::string, size_t>>
                  markdown_history_;                // (text, cursor) snapshots for undo
    // Last edit kind, so a run of typing or of deleting becomes one undo step.
    enum class MdEdit { None, Insert, Delete };
    MdEdit        md_last_edit_ = MdEdit::None;
    // Scroll offset in px, last full content height, and the per-line layout
    // map produced by the last render (used to map taps → cursor offset and to
    // draw the caret).
    double        markdown_scroll_     = 0.0;
    double        markdown_content_h_  = 0.0;
    double        md_scroll_at_press_  = 0.0;
    std::vector<MdLineBox> md_lines_;

    // Pen capture state
    Stroke        live_stroke_;
    bool          pen_down_       = false;
    InkRect       live_ink_bbox_  = {0, 0, 0, 0};   // union of A2 updates for current stroke

    // Eraser drag state: erase along the segment between consecutive samples
    // so a fast sweep doesn't leave gaps.
    bool          erase_active_   = false;
    double        erase_px_       = 0;
    double        erase_py_       = 0;

    LinkPicker    picker_;

    // PDF export progress (drawn as a modal bar; export runs on the main
    // thread and pumps the UI between pages).
    bool          exporting_pdf_  = false;
    int           export_done_    = 0;
    int           export_total_   = 0;

    // Finger gestures (stylus draws via evdev; fingers arrive as GTK pointer
    // events). Decided on release.
    bool          swipe_active_   = false;
    double        swipe_x0_       = 0;
    double        swipe_y0_       = 0;
    // Last time a stylus sample was processed. Page swipes are finger-only, so
    // a recent pen sample suppresses the swipe (the pen also emits pointer
    // events). 400 ms covers the gap between an evdev sample and the GTK
    // release without leaking into a genuine later finger swipe.
    uint32_t      last_pen_ms_    = 0;
    static constexpr uint32_t kPenSwipeGuardMs = 400;

    // Text input modal + delete confirmation. Only one is open at a time.
    bool          input_open_     = false;
    InputPurpose  input_purpose_  = InputPurpose::Rename;
    std::string   input_title_;        // modal heading, e.g. "Rename"
    TextEdit      input_;              // editable text
    std::string   input_target_id_;    // entry id being renamed
    bool          confirm_open_   = false;
    std::string   confirm_text_;       // e.g. "Delete \"Notes\"?"
    std::string   confirm_target_id_;
    TagScope      tags_scope_     = TagScope::Page;

    // Draw-to-attachment / OCR modal.
    bool          draw_open_      = false;
    DrawPurpose   draw_purpose_   = DrawPurpose::Image;
    std::vector<Stroke> draw_strokes_;
    Stroke        draw_live_;
    bool          draw_pen_down_  = false;
    bool          draw_erase_active_ = false;
    double        draw_erase_px_  = 0;
    double        draw_erase_py_  = 0;
    InkRect       draw_ink_bbox_  = {0, 0, 0, 0};
    // Live OCR candidate shown in the draw modal so the recognised text isn't
    // a surprise on Save. Recomputed after each stroke; "" until then.
    std::string   ocr_preview_;

    // Template picker modal.
    bool          tmpl_open_      = false;
    int           tmpl_scroll_    = 0;
    int           tmpl_sel_       = 0;
    std::vector<std::string> tmpl_custom_;   // PNG filenames in vault templates/

    // Settings modal + persisted config.
    bool          settings_open_  = false;
    std::string   config_path_;          // resolved once in load_config()
    std::string   export_dir_;           // "" = export next to the note (default)
    bool          watch_enabled_  = false;

    // Bluetooth keyboard page (over the settings modal).
    struct BtPage {
        bool        open      = false;
        bool        reached   = false;   // the daemon answered the last poll
        bool        polling   = false;   // a poll chain is in flight
        bool        starting  = false;   // we launched the service, waiting for it
        bool        scan_wait = false;   // we started a scan: collect its results
        BtStatus    st;
        std::vector<BtDevice> found;     // scan results, minus paired devices
        std::string pair_addr, pair_name;
        std::string pin, pin_before;     // passkey to type; one already in the log
        std::string note;                // outcome of the last action
        std::string forget_armed;        // address waiting for a second Forget
        int         sel   = 0;           // keyboard-selected row
        int         ticks = 0;
        guint       timer = 0;
        std::string sig;                 // what's on screen, to skip no-op repaints
    } bt_;

    // File-watch state: GLib timer source + last-seen mtimes / listing sig so
    // we only reload (and only fire an e-ink refresh) on a real change.
    guint         watch_source_   = 0;
    uint32_t      md_disk_mtime_  = 0;   // mtime of markdown_path_ at last load/save
    std::string   browser_sig_;          // signature of the current listing

    // Thread plumbing
    PenReader     pen_;
    Ocr           ocr_;
    std::mutex          q_mu_;
    std::queue<PenSample> pen_queue_;
    std::queue<OcrResult> ocr_queue_;

    std::string   notes_dir_     = "/mnt/us/documents/betternotes";
    std::string   tessdata_dir_;
    std::string   status_;
};

} // namespace bn
