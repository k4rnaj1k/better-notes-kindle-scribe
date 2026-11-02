// App core: startup and GTK glue, rendering, screen routing, save/export.

#include "app.h"

#include "canvas.h"
#include "inkfb.h"
#include "note_io.h"
#include "pages.h"
#include "pdf_export.h"
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bn {

namespace {

// GTK signal trampolines
gboolean cb_expose(GtkWidget *w, GdkEventExpose *e, gpointer self) {
    int win_w = w->allocation.width;
    int win_h = w->allocation.height;
    cairo_t *cr = gdk_cairo_create(w->window);
    // Clip to the exposed rectangle. gdk_cairo_create gives an unclipped
    // context, so without this on_draw rasterises the WHOLE window on every
    // partial redraw — re-rendering all page strokes even though X only pushes
    // the dirty rect to e-ink. Clipping lets cairo skip everything outside the
    // dirty region and lets canvas_render_page cull off-screen strokes.
    if (e && e->area.width > 0 && e->area.height > 0) {
        cairo_rectangle(cr, e->area.x, e->area.y, e->area.width, e->area.height);
        cairo_clip(cr);
    }
    static_cast<App *>(self)->on_draw(cr, win_w, win_h);
    cairo_destroy(cr);
    return TRUE;
}
gboolean cb_press(GtkWidget *, GdkEventButton *e, gpointer self) {
    auto *a = static_cast<App *>(self);
    double dx, dy; a->screen_to_drawing(e->x, e->y, dx, dy);
    return a->on_button_press(dx, dy) ? TRUE : FALSE;
}
gboolean cb_release(GtkWidget *, GdkEventButton *e, gpointer self) {
    auto *a = static_cast<App *>(self);
    double dx, dy; a->screen_to_drawing(e->x, e->y, dx, dy);
    return a->on_button_release(dx, dy) ? TRUE : FALSE;
}
gboolean cb_motion(GtkWidget *, GdkEventMotion *e, gpointer self) {
    auto *a = static_cast<App *>(self);
    double dx, dy; a->screen_to_drawing(e->x, e->y, dx, dy);
    return a->on_motion(dx, dy) ? TRUE : FALSE;
}
gboolean cb_key_press(GtkWidget *, GdkEventKey *e, gpointer self) {
    return static_cast<App *>(self)->on_gtk_key(e) ? TRUE : FALSE;
}
void cb_destroy(GtkWidget *, gpointer) { gtk_main_quit(); }

} // namespace

App &App::instance() {
    static App a;
    return a;
}

App::App() = default;

int App::run(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // UI orientation; see rotation_. Override with BN_ROTATION=0/90/180/270.
    if (const char *r = std::getenv("BN_ROTATION")) {
        set_rotation(std::atoi(r));
    }

    // Direct-to-framebuffer ink path. OPT-IN via BN_ENABLE_INKFB=1: under
    // the Kindle's X server, direct /dev/fb0 writes can be composited over
    // or land in the wrong buffer, so this is off by default. When enabled
    // it runs *in addition* to the GTK partial redraw, giving instant A2
    // feedback while the cairo path still guarantees a correct final image.
    if (std::getenv("BN_ENABLE_INKFB")) {
        inkfb_init(rotation_);
    }

    // Persisted settings (home folder / export folder / watch toggle) override
    // the CLI --notes-dir default, so a folder chosen in-app sticks.
    load_config();
    hid_.set_layout(layout_);
    keyboard_.set_layout(layout_);

    index_.open(notes_dir_);
    canvas_set_vault_root(notes_dir_);   // resolves custom-template bg_image paths
    ocr_.set_tessdata_dir(tessdata_dir_);
    ocr_.set_callback([this](const OcrResult &r){
        std::lock_guard<std::mutex> g(q_mu_);
        ocr_queue_.push(r);
    });

    pen_.start([this](const PenSample &s){ on_pen_sample(s); });

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_),
        "L:A_N:application_PC:N_ID:com.kindle.betternotes");
    gtk_window_fullscreen(GTK_WINDOW(window_));
    g_signal_connect(window_, "destroy", G_CALLBACK(cb_destroy), nullptr);
    // Keyboards the X server adopted itself (see on_gtk_key).
    g_signal_connect(window_, "key-press-event", G_CALLBACK(cb_key_press), this);

    canvas_ = gtk_drawing_area_new();
    gtk_widget_add_events(canvas_,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);
    g_signal_connect(canvas_, "expose-event",
                     G_CALLBACK(cb_expose), this);
    g_signal_connect(canvas_, "button-press-event",
                     G_CALLBACK(cb_press), this);
    g_signal_connect(canvas_, "button-release-event",
                     G_CALLBACK(cb_release), this);
    g_signal_connect(canvas_, "motion-notify-event",
                     G_CALLBACK(cb_motion), this);
    gtk_container_add(GTK_CONTAINER(window_), canvas_);
    gtk_widget_show_all(window_);

    // Every keyboard funnels into handle_key: the on-screen one here, the
    // Bluetooth / USB ones through hid_ (and X, via cb_key_press).
    keyboard_.on_key([this](const KeyEvent &e){ handle_key(e); });
    hid_.start([this](const KeyEvent &e){ handle_key(e); },
               [this](const std::string &name, bool added){
                   on_hw_keyboard(name, added);
               });

    // The pen thread wakes the main loop directly via g_idle_add when a
    // sample arrives (see on_pen_sample), so there's no polling timer.
    enter_browser();
    if (watch_enabled_) start_file_watch();
    gtk_main();

    stop_file_watch();
    flush_markdown();
    if (note_.dirty) save_current();
    hid_.stop();
    ocr_.stop();
    pen_.stop();
    inkfb_close();
    return 0;
}

void App::quit() {
    // Persist outstanding work before tearing down. The rest of the shutdown
    // (ocr/pen/inkfb stop) runs in run()'s epilogue after gtk_main() returns.
    flush_markdown();
    if (note_.dirty) save_current();
    gtk_main_quit();
}

// ---------------------------------------------------------------- repaint

void App::redraw() {
    if (canvas_) gtk_widget_queue_draw(canvas_);
}

void App::redraw_rect(double x, double y, double w, double h) {
    if (!canvas_) return;
    // Map drawing-space rect → window (X11) rect by applying the same
    // forward rotation cairo uses in on_draw.
    double wx, wy, ww, wh;
    switch (rotation_) {
    case 90:
        wx = (double)xw_ - (y + h); wy = x; ww = h; wh = w;
        break;
    case 180:
        wx = (double)xw_ - (x + w); wy = (double)xh_ - (y + h); ww = w; wh = h;
        break;
    case 270:
        wx = y; wy = (double)xh_ - (x + w); ww = h; wh = w;
        break;
    default:
        wx = x; wy = y; ww = w; wh = h;
        break;
    }
    // Inflate slightly so antialiased edges aren't clipped.
    const int pad = 2;
    int ix = (int)std::floor(wx) - pad;
    int iy = (int)std::floor(wy) - pad;
    int iw = (int)std::ceil(ww) + 2 * pad;
    int ih = (int)std::ceil(wh) + 2 * pad;
    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    if (iw <= 0 || ih <= 0) return;
    gtk_widget_queue_draw_area(canvas_, ix, iy, iw, ih);
}

void App::redraw_toolbar() {
    // Partial repaint of just the toolbar strip at the top. A full redraw()
    // would trigger a slow full-screen e-ink refresh for every button tap
    // (and for show/hide), which makes the UI feel sluggish. Always cover
    // the full laid-out toolbar height so toggling visibility cleanly
    // repaints the whole strip (toolbar ↔ revealed page content).
    redraw_rect(0, 0, (double)win_w_, toolbar_.full_height());
}

void App::redraw_pen_menu_region() {
    // Repaint the toolbar strip plus only the dropdown panel beneath it. The
    // page beside the narrow panel is untouched, so opening/closing the pen
    // menu doesn't re-render all strokes or fire a full-width e-ink refresh.
    redraw_toolbar();
    Rect p = toolbar_.pen_menu_panel_rect();
    if (p.w > 0 && p.h > 0) redraw_rect(p);
}

void App::redraw_kbd_after_press() {
    Rect r = keyboard_.last_key_rect();
    if (r.w > 0) redraw_rect(r);
}

void App::redraw_kbd_after_release() {
    if (keyboard_.last_full_redraw()) redraw_keyboard_band();
    else                              redraw_kbd_after_press();
}

void App::redraw_keyboard_band() {
    redraw_rect(0, keyboard_.top_y(),
                (double)win_w_, (double)win_h_ - keyboard_.top_y());
}

Rect App::show_tab_rect() const {
    // Small tab at the top-right corner, shown when the toolbar is hidden.
    double w = 96.0, h = 56.0;
    return Rect{(double)win_w_ - w - 12.0, 0.0, w, h};
}

void App::draw_show_tab(cairo_t *cr) {
    using namespace theme;
    // A pill hanging from the top edge: rounded at the bottom only (the top
    // corners are pushed off-screen), with a down chevron.
    Rect r = show_tab_rect();
    Rect body{r.x, r.y - kRadius, r.w, r.h + kRadius - kShadowDy};
    ui_card(cr, body, kRadius, kPaper);
    double cx = r.x + r.w / 2.0, cy = r.y + (r.h - kShadowDy) / 2.0, s = 12.0;
    ui_grey(cr, kInk);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, cx - s, cy - s * 0.4);
    cairo_line_to(cr, cx, cy + s * 0.4);
    cairo_line_to(cr, cx + s, cy - s * 0.4);
    cairo_stroke(cr);
}

// ------------------------------------------------------------ page cache

cairo_surface_t *App::page_surface(int page) {
    if (page < 0 || page >= (int)note_.pages.size()) return nullptr;
    if (win_w_ <= 0 || win_h_ <= 0) return nullptr;

    // Reset the whole cache if the note or drawing geometry changed (bitmaps
    // are sized to win_w_ x win_h_ and indexed within one note).
    if (page_cache_note_ != note_.id ||
        page_cache_w_ != win_w_ || page_cache_h_ != win_h_) {
        clear_page_cache();
        page_cache_note_ = note_.id;
        page_cache_w_ = win_w_;
        page_cache_h_ = win_h_;
    }

    for (size_t i = 0; i < page_cache_.size(); ++i) {
        if (page_cache_[i].page == page) {
            if (i != 0)   // promote to most-recently-used
                std::rotate(page_cache_.begin(), page_cache_.begin() + i,
                            page_cache_.begin() + i + 1);
            return page_cache_.front().surf;
        }
    }

    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, win_w_, win_h_);
    if (!s) return nullptr;
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    cairo_t *ic = cairo_create(s);
    // Match the on-screen ink exactly (hard-edged, like the inkfb / live
    // preview) so a cached page never looks different from a freshly-drawn one.
    cairo_set_antialias(ic, CAIRO_ANTIALIAS_NONE);
    canvas_render_page(ic, note_.pages[page], (double)win_w_, (double)win_h_);
    cairo_destroy(ic);

    page_cache_.insert(page_cache_.begin(), PageCache{page, s});
    while ((int)page_cache_.size() > kPageCacheMax) {
        cairo_surface_destroy(page_cache_.back().surf);
        page_cache_.pop_back();
    }
    return s;
}

void App::update_page_cache_region(int page, const Rect &r) {
    if (page < 0 || page >= (int)note_.pages.size()) return;
    if (r.w <= 0 || r.h <= 0) return;
    cairo_surface_t *s = nullptr;
    for (auto &e : page_cache_)
        if (e.page == page) { s = e.surf; break; }
    if (!s) return;   // not cached yet — a fresh render will include the change
    cairo_t *ic = cairo_create(s);
    cairo_set_antialias(ic, CAIRO_ANTIALIAS_NONE);
    cairo_rectangle(ic, r.x, r.y, r.w, r.h);
    cairo_clip(ic);
    // canvas_render_page repaints the template (clearing the region) then the
    // strokes culled to the clip, so the cached bitmap mirrors the page again.
    canvas_render_page(ic, note_.pages[page], (double)win_w_, (double)win_h_);
    cairo_destroy(ic);
}

void App::invalidate_page_cache(int page) {
    for (size_t i = 0; i < page_cache_.size(); ++i)
        if (page_cache_[i].page == page) {
            cairo_surface_destroy(page_cache_[i].surf);
            page_cache_.erase(page_cache_.begin() + i);
            return;
        }
}

void App::clear_page_cache() {
    for (auto &e : page_cache_) cairo_surface_destroy(e.surf);
    page_cache_.clear();
}

// ---------------------------------------------------------- screen routing

void App::enter_browser() {
    if (note_.dirty) save_current();
    flush_markdown();
    screen_ = Screen::Browser;
    index_.open(notes_dir_, browser_path_);
    browser_.set_current_path(browser_path_);
    browser_scroll_ = 0.0;
    // Keep keyboard focus on a tile when a keyboard is connected; a
    // finger-only user never sees the ring.
    browser_.set_focus(hw_keyboard() ? 0 : -1);
    browser_sig_ = browser_signature();   // file-watch baseline for this listing
    redraw();
}

void App::enter_folder(const std::string &rel_path) {
    browser_path_ = rel_path;
    enter_browser();
}

void App::enter_parent_folder() {
    if (browser_path_.empty()) return;
    auto slash = browser_path_.find_last_of('/');
    browser_path_ = (slash == std::string::npos) ? "" : browser_path_.substr(0, slash);
    enter_browser();
}

void App::enter_note(const std::string &id) {
    if (note_.dirty) save_current();
    flush_markdown();   // persist + rename a markdown we may be leaving (e.g. Back)
    std::string dir = notes_dir_ + "/" + id;
    Note n;
    if (!load_note(dir, n)) {
        log_err("failed to load %s", dir.c_str());
        return;
    }
    note_ = std::move(n);
    clear_page_cache();   // stale bitmaps belong to the previous note
    // Canonicalise: the id stored in-memory is always the vault-relative
    // path, regardless of what was on disk. save_current concatenates
    // notes_dir_ + "/" + note_.id, so it needs this form.
    note_.id = id;
    current_page_ = 0;
    history_.push({note_.id, current_page_});
    screen_ = Screen::NoteView;
    redraw();
}

void App::enter_markdown(const std::string &path) {
    flush_markdown();   // persist + rename the markdown we're leaving
    markdown_path_ = path;
    std::string buf;
    read_file(path, buf);
    md_.set(std::move(buf));     // cursor at the end
    markdown_history_.clear();
    md_last_edit_ = MdEdit::None;
    markdown_dirty_ = false;
    markdown_scroll_ = 0.0;
    md_lines_.clear();
    ensure_markdown_title();   // make sure the inline title heading is present
    md_.doc_end();
    md_disk_mtime_ = file_mtime_ms(path);   // file-watch baseline
    screen_ = Screen::Markdown;
    redraw();
}

void App::go_back() {
    if (history_.can_back()) {
        auto e = history_.pop_back_to_prev();
        enter_note(e.note_id);
        current_page_ = e.page;
    } else {
        enter_browser();
    }
    redraw();
}

void App::flip_page(int delta) {
    if (screen_ != Screen::NoteView) return;
    int np = pages_clamp(note_, current_page_ + delta);
    if (np != current_page_) { current_page_ = np; redraw(); }
}

// --------------------------------------------------------------- note ops

void App::save_current() {
    if (note_.id.empty()) return;
    std::string dir = notes_dir_ + "/" + note_.id;
    if (save_note(dir, note_)) {
        index_.touch(note_.id, note_.title);
        index_.save();
        note_.dirty = false;
        status_ = "saved";
    } else {
        status_ = "save failed";
    }
}

void App::export_current_pdf() {
    if (note_.id.empty()) return;
    // Default: write the PDF beside the note inside the vault. If a default
    // export folder is configured, write a slugified <title>.pdf there instead.
    // Name the file after the note's current title, not note_.id: the id is
    // the slug fixed at creation time ("untitled" for a never-renamed note).
    std::string base = slugify(note_.title.empty() ? note_.id : note_.title);
    std::string out;
    if (!export_dir_.empty()) {
        ensure_dir(export_dir_);
        out = export_dir_ + "/" + base + ".pdf";
    } else {
        // Beside the note inside the vault (same parent folder as the note dir).
        std::string parent = note_.id;
        auto slash = parent.find_last_of('/');
        parent = (slash == std::string::npos) ? "" : parent.substr(0, slash);
        out = notes_dir_ + (parent.empty() ? "" : "/" + parent) + "/" + base + ".pdf";
    }
    // Strokes are stored in full-screen page-pixel space. Derive the PDF
    // page size from that aspect ratio (uniform scale) so the export isn't
    // squashed — a landscape note exports to a landscape page, a portrait
    // note to a portrait page. Cap the long edge at the A4 long dimension.
    double src_w = win_w_ > 0 ? (double)win_w_ : 1404.0;
    double src_h = win_h_ > 0 ? (double)win_h_ : 1872.0;
    const double A4_LONG = 842.0;   // pt
    double scale = A4_LONG / std::max(src_w, src_h);
    double page_w = src_w * scale;
    double page_h = src_h * scale;

    // Export runs synchronously on the main thread; drive a modal progress
    // bar by repainting + pumping the GTK loop between pages. Input is gated
    // off (pointer, keys and pen) while exporting_pdf_ is set, so the
    // re-entrant pump can't mutate the note mid-write.
    exporting_pdf_ = true;
    export_total_  = (int)std::max<size_t>(1, note_.pages.size());
    export_done_   = 0;
    status_        = "exporting pdf…";
    redraw();   // lay down the dim backdrop + initial bar
    while (gtk_events_pending()) gtk_main_iteration();

    bool ok = export_pdf(out, note_, page_w, page_h, src_w, src_h,
        note_.title.empty() ? note_.id : note_.title,
        [this](int done, int total) {
            export_done_  = done;
            export_total_ = std::max(1, total);
            redraw_rect(export_overlay_rect());   // update just the card
            while (gtk_events_pending()) gtk_main_iteration();
        });

    exporting_pdf_ = false;
    status_ = ok ? ("pdf → " + out) : "pdf failed";
    redraw();   // clear the overlay
}

// --------------------------------------------------------------- rotation

void App::set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) deg = 0;
    rotation_ = deg;
    redraw();
}

void App::screen_to_drawing(double sx, double sy, double &dx, double &dy) const {
    // Inverse of the cairo transform applied in on_draw(). xw_/xh_ are
    // the X11 window dims; output dx/dy is in drawing space.
    switch (rotation_) {
    case 90:  dx = sy;                  dy = (double)xw_ - sx;       break;
    case 180: dx = (double)xw_ - sx;    dy = (double)xh_ - sy;       break;
    case 270: dx = (double)xh_ - sy;    dy = sx;                     break;
    default:  dx = sx;                  dy = sy;                     break;
    }
}

// -------------------------------------------------------------- rendering

void App::on_draw(cairo_t *cr, int win_w, int win_h) {
    xw_ = win_w; xh_ = win_h;
    win_w_ = draw_w(); win_h_ = draw_h();

    // Software-rotate the cairo context so the rest of the renderer can
    // assume drawing-space coordinates. The Kindle X server doesn't expose
    // XRandR, so this is the only way to get a properly oriented UI.
    switch (rotation_) {
    case 90:
        cairo_translate(cr, xw_, 0);
        cairo_rotate(cr, M_PI / 2.0);
        break;
    case 180:
        cairo_translate(cr, xw_, xh_);
        cairo_rotate(cr, M_PI);
        break;
    case 270:
        cairo_translate(cr, 0, xh_);
        cairo_rotate(cr, -M_PI / 2.0);
        break;
    }

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    // From here on, use the drawing-space dims.
    win_w = win_w_; win_h = win_h_;

    // Lay out the keyboard up front so screens that need its top edge this
    // frame (markdown body clip + scroll) see valid geometry.
    if (keyboard_.visible()) keyboard_.layout(win_w, win_h);

    if (screen_ == Screen::Browser) {
        browser_.layout(win_w, win_h, index_.entries().size());
        browser_.draw(cr, index_, win_w, win_h, browser_scroll_,
                      move_active_, move_title_);
        // Settings modal sits over the browser; editing a path swaps it for the
        // input modal (drawn on top), so show settings only when no text field
        // is open.
        if (settings_open_ && !input_open_) {
            if (bt_.open) draw_bluetooth(cr, win_w, win_h);
            else          draw_settings_modal(cr, win_w, win_h);
        }
        if (input_open_) {
            draw_input_modal(cr);
            keyboard_.draw(cr);
        }
        if (confirm_open_) draw_confirm_modal(cr, win_w, win_h);
        return;
    }

    toolbar_.layout(win_w, screen_ == Screen::Markdown
                               ? ToolbarMode::Markdown : ToolbarMode::Note);
    if (screen_ == Screen::NoteView) draw_note_view(cr, win_w, win_h);
    else                             draw_markdown_view(cr, win_w, win_h);

    // A text dialog dims the page, toolbar included; the keyboard stays on top.
    if (!input_open_) keyboard_.draw(cr);   // no-op when hidden

    // Floating "show toolbar" tab whenever the toolbar is hidden.
    if (!toolbar_.visible()) draw_show_tab(cr);

    // The layout badge only means something when there's a keyboard to type on.
    bool typing = hw_keyboard() || keyboard_.visible();
    toolbar_.draw(cr, tool_, status_, current_page_,
                  (int)std::max<size_t>(1, note_.pages.size()),
                  typing ? layout_badge(layout_) : nullptr);

    // Modals, bottom to top.
    if (picker_.open)  draw_link_picker(cr, win_w, win_h);
    if (tmpl_open_)    draw_template_picker(cr, win_w, win_h);
    if (input_open_) {                             // tags editor over a note
        draw_input_modal(cr);
        keyboard_.draw(cr);
    }
    if (draw_open_)    draw_draw_modal(cr, win_w, win_h);
    if (exporting_pdf_) draw_export_overlay(cr, win_w, win_h);
}

void App::draw_note_view(cairo_t *cr, int win_w, int win_h) {
    // Full-screen page; the toolbar overlays the top when visible. Render
    // strokes WITHOUT antialiasing so the committed/redrawn ink matches the
    // hard-edged live ink the inkfb fast path draws (with AA on, strokes
    // visibly "smoothed out" on the next redraw). PDF export uses its own
    // context and keeps AA.
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    if (current_page_ < (int)note_.pages.size()) {
        // Blit the cached page bitmap (cheap, clipped to the exposed rect)
        // instead of re-rasterising every stroke. Falls back to a direct
        // render if the bitmap couldn't be allocated.
        if (cairo_surface_t *cached = page_surface(current_page_)) {
            cairo_set_source_surface(cr, cached, 0, 0);
            // NEAREST: the cache is 1:1 with the drawing space, so for the
            // 90/180/270 rotations this is an exact transpose, no blur.
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint(cr);
        } else {
            canvas_render_page(cr, note_.pages[current_page_], win_w, win_h);
        }
    }
    if (pen_down_ && tool_.current == Tool::Pen)
        canvas_render_stroke(cr, live_stroke_);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    // Lasso preview
    if (tool_.current == Tool::Lasso && lasso_.pts.size() > 1) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_set_line_width(cr, 1.0);
        static const double dashes[] = {4, 3};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_move_to(cr, lasso_.pts[0].x, lasso_.pts[0].y);
        for (size_t i = 1; i < lasso_.pts.size(); ++i)
            cairo_line_to(cr, lasso_.pts[i].x, lasso_.pts[i].y);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }
    // Link overlays
    for (auto &l : note_.links) {
        if (l.page != current_page_) continue;
        cairo_set_source_rgba(cr, 0, 0, 1, 0.05);
        cairo_rectangle(cr, l.rect.x, l.rect.y, l.rect.w, l.rect.h);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 1, 0.4);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
}

void App::draw_markdown_view(cairo_t *cr, int win_w, int win_h) {
    (void)win_h;
    double top    = markdown_band_top();
    double bottom = markdown_band_bottom();
    // Clip to the text band so scrolled-out lines don't bleed over the
    // toolbar or keyboard.
    cairo_save(cr);
    cairo_rectangle(cr, 0, top, win_w, bottom - top);
    cairo_clip(cr);

    if (tool_.markdown_pretty) {
        // Read-oriented preview: formatted markdown, no editable caret.
        // Images resolve relative to the .md file's own directory.
        md_lines_.clear();
        std::string base = markdown_path_;
        auto slash = base.find_last_of('/');
        base = (slash == std::string::npos) ? "." : base.substr(0, slash);
        double y = render_markdown_pretty(cr, 24, top - markdown_scroll_,
                                          win_w - 48, md_.text, base);
        markdown_content_h_ = (y + markdown_scroll_) - top;
    } else {
        double y = render_markdown(cr, 24, top - markdown_scroll_,
                                   win_w - 48, md_.text, &md_lines_);
        markdown_content_h_ = (y + markdown_scroll_) - top;

        // Caret: a thin black bar at the cursor's exact layout position.
        double cx, cy0, cy1;
        if (markdown_caret(cr, md_.text, md_lines_, md_.cursor,
                           &cx, &cy0, &cy1)) {
            // Keep the caret on-screen while typing/navigating. Adjust
            // scroll for the NEXT frame rather than re-laying out here.
            if (cy0 < top) {
                markdown_scroll_ -= (top - cy0) + 24.0;
                clamp_markdown_scroll();
                redraw_markdown_body();
            } else if (cy1 > bottom) {
                markdown_scroll_ += (cy1 - bottom) + 24.0;
                clamp_markdown_scroll();
                redraw_markdown_body();
            } else {
                cairo_set_source_rgb(cr, 0, 0, 0);
                cairo_set_line_width(cr, 2.0);
                cairo_move_to(cr, cx, cy0);
                cairo_line_to(cr, cx, cy1);
                cairo_stroke(cr);
            }
        }
    }
    cairo_restore(cr);
}

} // namespace bn
