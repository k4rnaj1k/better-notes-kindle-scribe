#pragma once
#include "filebrowser.h"
#include "index.h"
#include "inkfb.h"
#include "keyboard.h"
#include "lasso.h"
#include "links.h"
#include "markdown.h"
#include "ocr.h"
#include "pen.h"
#include "strokes.h"
#include "toolbar.h"
#include "tools.h"

#include <gtk/gtk.h>

#include <memory>
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
    std::vector<IndexEntry> entries;  // snapshot of vault at picker open time
};

class App {
public:
    static App &instance();

    void set_notes_dir(const std::string &d) { notes_dir_ = d; }
    void set_tessdata_dir(const std::string &d) { tessdata_dir_ = d; }

    int run(int argc, char *argv[]);

    // Public for the GTK signal trampolines.
    void on_draw   (cairo_t *cr, int win_w, int win_h);
    bool on_button_press (double x, double y);
    bool on_button_release(double x, double y);
    bool on_motion(double x, double y);

    void draw_link_picker(cairo_t *cr, int win_w, int win_h);
    void draw_export_overlay(cairo_t *cr, int win_w, int win_h);  // PDF progress
    void draw_input_modal(cairo_t *cr, int win_w, int win_h);     // rename/tags input
    void draw_confirm_modal(cairo_t *cr, int win_w, int win_h);   // delete confirm
    void draw_draw_modal(cairo_t *cr, int win_w, int win_h);      // draw/OCR canvas
    void draw_template_picker(cairo_t *cr, int win_w, int win_h); // template chooser
    void draw_show_tab(cairo_t *cr, int win_w);   // "show toolbar" tab when hidden
    Rect show_tab_rect() const;                    // hit region for that tab

    // X11 window coords → drawing-space coords (inverse of the cairo
    // rotation applied in on_draw). Public: called by the GTK trampolines.
    void screen_to_drawing(double sx, double sy, double &dx, double &dy) const;

    // Called from pen_reader thread; just enqueues onto pen_queue_.
    void on_pen_sample(const PenSample &s);

    // GTK idle callback drains pen_queue_ + ocr_queue_.
    void process_async_events();

    // GTK timer callback for the file-watch poller. Public for the trampoline.
    void on_watch_tick();               // periodic poll (browser + markdown)

private:
    App();

    void handle_picker_press(double x, double y);

    // Markdown helpers
    void markdown_snapshot();
    void markdown_undo();
    void markdown_insert(const std::string &text);
    void markdown_backspace();
    void redraw_markdown_body();          // partial repaint of just the text band
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

    // --- screen routing ---
    void enter_browser();
    void enter_folder(const std::string &rel_path);  // descends in vault
    void enter_parent_folder();                       // ".." navigation
    void enter_note(const std::string &id);
    void enter_markdown(const std::string &path);

    // --- browser interactions ---
    void handle_browser_action(const BrowserHit &h);
    void open_new_folder();          // input modal for a new subfolder name
    void clamp_browser_scroll();

    // --- note ops ---
    void save_current();
    void export_current_pdf();
    Rect export_overlay_rect() const;   // centred progress card

    // Browser rename / tags input modal. The on-screen keyboard edits
    // input_buf_; commit_input() dispatches on input_purpose_.
    enum class InputPurpose { Rename, Tags, NewFolder, HomeFolder, ExportFolder };
    void open_rename(const std::string &id, const std::string &cur_title);
    void commit_input();   // apply the rename/tags, close the modal
    void close_input();    // dismiss without applying
    Rect modal_card_rect() const;       // shared centred card geometry
    Rect modal_btn_rect(bool primary) const;  // Save/Delete (primary) | Cancel

    // --- tags (#8): edit per-page or notebook tags via the input modal ---
    enum class TagScope { Page, Notebook };
    void open_tags_editor();
    void commit_tags();                 // parse input_buf_ into the active scope
    void load_tags_into_input();        // refill input_buf_ from the active scope
    Rect tags_scope_btn_rect() const;   // Page↔Notebook toggle inside the modal
    static std::vector<std::string> parse_tags(const std::string &csv);
    static std::string join_tags(const std::vector<std::string> &tags);

    // --- draw modal (#11 drawings / #9 OCR), markdown only ---
    enum class DrawPurpose { Image, Ocr };
    void open_draw_modal(DrawPurpose purpose);
    void close_draw_modal();
    void commit_draw_modal();
    void handle_draw_sample(const PenSample &s);
    Rect draw_canvas_rect() const;
    Rect draw_btn_rect(int which) const;   // 0=Save 1=Clear 2=Cancel
    // Rasterise draw_strokes_ (cropped to their bbox + padding) to a PNG.
    // Returns false if there's nothing drawn or the write fails.
    bool draw_render_png(const std::string &abs_path);

    // --- template picker (#10) ---
    void open_template_picker();
    void apply_template(TemplateId tmpl, const std::string &bg_image);
    void handle_template_picker_press(double x, double y);

    // --- settings (home folder, export folder, file-watch toggle) ---
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

    // --- file watch (Syncthing): poll the open file / browser for external
    // updates and reload, so background syncs show up without a restart. ---
    void start_file_watch();
    void stop_file_watch();
    std::string browser_signature() const;   // cheap "did the listing change?"

    // --- pen sample → page coordinates ---
    void map_pen_to_page(const PenSample &s, double &px, double &py);

    void redraw();
    void redraw_rect(double x, double y, double w, double h);
    void redraw_toolbar();   // fast partial repaint of the toolbar strip
    void redraw_pen_menu_region();   // toolbar strip + pen dropdown panel only
    // Keyboard feedback: repaint only the key that changed so taps don't flash
    // the whole on-screen keyboard (full band only on Shift/Mode switches).
    void redraw_kbd_after_press();
    void redraw_kbd_after_release();
    void redraw_input_card();   // repaint just the rename/tags modal card

    // --- page render cache (NoteView) ---
    // Rendered page bitmaps (template + committed strokes) reused across
    // redraws, so flipping back to a page or any partial repaint is a cheap
    // blit instead of re-rasterising every stroke (the screen analogue of the
    // PDF "render once to an image" path). The current page's bitmap is updated
    // incrementally on draw/erase; others stay read-only until revisited.
    cairo_surface_t *page_surface(int page);            // get-or-render bitmap
    void update_page_cache_region(int page, const Rect &r);  // incremental repaint
    void invalidate_page_cache(int page);               // drop one page's bitmap
    void clear_page_cache();                             // drop all bitmaps

    // Software rotation. The Kindle's X server doesn't support XRandR, so
    // we pre-rotate the cairo context and inverse-rotate input events.
    // 0 / 90 / 180 / 270 are supported. Default 90 = portrait on Scribe.
    int  rotation_deg() const { return rotation_; }
    void set_rotation(int deg);

    // Drawing-space dimensions (after rotation). For 90/270, w/h are
    // swapped relative to the X window allocation.
    int  draw_w() const { return (rotation_ == 90 || rotation_ == 270) ? xh_ : xw_; }
    int  draw_h() const { return (rotation_ == 90 || rotation_ == 270) ? xw_ : xh_; }

    // --- state ---
    GtkWidget    *window_  = nullptr;
    GtkWidget    *canvas_  = nullptr;
    int           xw_      = 0;     // raw X11 window size (landscape on Scribe)
    int           xh_      = 0;
    int           win_w_   = 0;     // post-rotation drawing-space size (portrait)
    int           win_h_   = 0;
    // The Scribe's X server is portrait-native, so 0 keeps the UI portrait.
    // (rotation=90 rotated it INTO landscape.) Override with BN_ROTATION.
    int           rotation_ = 0;

    Screen        screen_  = Screen::Browser;
    ToolState     tool_;
    Toolbar       toolbar_;
    FileBrowser   browser_;
    Keyboard      keyboard_;
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
    std::string   markdown_path_;
    std::string   markdown_buf_;
    bool          markdown_dirty_ = false;
    size_t        markdown_cursor_ = 0;             // byte offset into buf
    std::vector<std::pair<std::string, size_t>>
                  markdown_history_;                // (buf, cursor) snapshots for undo
    // Markdown scroll/layout: scroll offset in px, last full content height,
    // and the per-line layout map produced by the last render (used to map
    // taps → cursor offset and to draw the caret).
    double        markdown_scroll_     = 0.0;
    double        markdown_content_h_  = 0.0;
    double        md_scroll_at_press_  = 0.0;
    std::vector<MdLineBox> md_lines_;

    // Pen capture state
    Stroke        live_stroke_;
    bool          pen_down_       = false;
    PenButton     active_button_  = PenButton::None;
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

    // Finger-swipe page navigation (stylus draws via evdev; finger swipes
    // here through GTK pointer events). Decided on release.
    bool          swipe_active_   = false;
    double        swipe_x0_       = 0;
    double        swipe_y0_       = 0;
    // Last time a stylus sample was processed. Page swipes are finger-only, so
    // a recent pen sample suppresses the swipe (the pen also emits pointer
    // events). 400 ms covers the gap between an evdev sample and the GTK
    // release without leaking into a genuine later finger swipe.
    uint32_t      last_pen_ms_    = 0;
    static constexpr uint32_t kPenSwipeGuardMs = 400;

    // Browser modals: rename (text input via the on-screen keyboard) and a
    // delete confirmation. Only one is open at a time.
    bool          input_open_     = false;
    std::string   input_title_;        // modal heading, e.g. "Rename"
    std::string   input_buf_;          // editable text
    std::string   input_target_id_;    // entry id being renamed
    bool          confirm_open_   = false;
    std::string   confirm_text_;       // e.g. "Delete \"Notes\"?"
    std::string   confirm_target_id_;
    InputPurpose  input_purpose_  = InputPurpose::Rename;

    // Tags editor state (shares the input modal/keyboard).
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
    std::vector<std::string> tmpl_custom_;   // PNG filenames in vault templates/

    // Settings modal + persisted config.
    bool          settings_open_  = false;
    std::string   config_path_;          // resolved once in load_config()
    std::string   export_dir_;           // "" = export next to the note (default)
    bool          watch_enabled_  = false;

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
