#include "app.h"

#include "canvas.h"
#include "inkfb.h"
#include "markdown.h"
#include "note_io.h"
#include "pages.h"
#include "pdf_export.h"
#include "templates.h"

#include <pango/pangocairo.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bn {

namespace {

constexpr double TOUCH_PRESSURE_THRESHOLD = 1.0;

// GTK signal trampolines (file-static)
gboolean cb_expose(GtkWidget *w, GdkEventExpose *, gpointer self) {
    int win_w = w->allocation.width;
    int win_h = w->allocation.height;
    cairo_t *cr = gdk_cairo_create(w->window);
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
void cb_destroy(GtkWidget *, gpointer) { gtk_main_quit(); }

gboolean cb_process_now(gpointer self) {
    static_cast<App *>(self)->process_async_events();
    // G_SOURCE_REMOVE / G_SOURCE_CONTINUE landed in GLib 2.32; the Kindle
    // sysroot is older, so use the literal it expands to (FALSE = remove).
    return FALSE;
}

} // namespace

App &App::instance() {
    static App a;
    return a;
}

App::App() = default;

void App::redraw() {
    if (canvas_) gtk_widget_queue_draw(canvas_);
}

Rect App::show_tab_rect() const {
    // Small tab at the top-right corner, shown when the toolbar is hidden.
    double w = 84.0, h = 52.0;
    return Rect{(double)win_w_ - w - 8.0, 0.0, w, h};
}

void App::draw_show_tab(cairo_t *cr, int win_w) {
    (void)win_w;
    Rect r = show_tab_rect();
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_stroke(cr);
    // Down chevron = "pull the toolbar back down".
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, s = 14.0;
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx - s, cy - s * 0.4);
    cairo_line_to(cr, cx, cy + s * 0.4);
    cairo_line_to(cr, cx + s, cy - s * 0.4);
    cairo_stroke(cr);
}

void App::redraw_toolbar() {
    // Partial repaint of just the toolbar strip at the top. A full redraw()
    // would trigger a slow full-screen e-ink refresh for every button tap
    // (and for show/hide), which makes the UI feel sluggish. Always cover
    // the full laid-out toolbar height so toggling visibility cleanly
    // repaints the whole strip (toolbar ↔ revealed page content).
    redraw_rect(0, 0, (double)win_w_, toolbar_.full_height());
}

void App::redraw_rect(double x, double y, double w, double h) {
    if (!canvas_) return;
    // Map drawing-space rect → window (X11) rect by applying the same
    // forward rotation cairo uses in on_draw.
    double wx, wy, ww, wh;
    switch (rotation_) {
    case 90:
        // Drawing (x,y,w,h) → window rect with x/y swapped and y flipped.
        wx = (double)xw_ - (y + h);
        wy = x;
        ww = h;
        wh = w;
        break;
    case 180:
        wx = (double)xw_ - (x + w);
        wy = (double)xh_ - (y + h);
        ww = w;
        wh = h;
        break;
    case 270:
        wx = y;
        wy = (double)xh_ - (x + w);
        ww = h;
        wh = w;
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

int App::run(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // Default 90° CW for the Scribe (X server reports landscape; we render
    // portrait via cairo rotation). Override with BN_ROTATION=0/90/180/270.
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

    index_.open(notes_dir_);
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

    keyboard_.on_text([this](const std::string &t){
        if (input_open_) { input_buf_ += t; redraw(); return; }
        if (screen_ == Screen::Markdown) {
            markdown_insert(t);
            redraw();
        }
    });
    keyboard_.on_key([this](const std::string &k){
        if (input_open_) {
            if (k == "Backspace") {
                if (!input_buf_.empty()) input_buf_.pop_back();
            } else if (k == "Enter") {
                commit_input();   // applies rename + redraws
                return;
            }
            redraw();
            return;
        }
        if (screen_ != Screen::Markdown) return;
        if (k == "Backspace") {
            markdown_backspace();
        } else if (k == "Enter") {
            // Snapshot before each new line so undo lands on a sensible
            // boundary, then insert the newline at the cursor.
            markdown_snapshot();
            markdown_insert("\n");
        } else if (k == "Left") {
            if (markdown_cursor_ > 0) --markdown_cursor_;
        } else if (k == "Right") {
            if (markdown_cursor_ < markdown_buf_.size()) ++markdown_cursor_;
        } else if (k == "Home") {
            // Jump to start of current line.
            size_t nl = markdown_buf_.rfind('\n', markdown_cursor_ > 0
                                            ? markdown_cursor_ - 1 : 0);
            markdown_cursor_ = (nl == std::string::npos) ? 0 : nl + 1;
        } else if (k == "End") {
            size_t nl = markdown_buf_.find('\n', markdown_cursor_);
            markdown_cursor_ = (nl == std::string::npos)
                ? markdown_buf_.size() : nl;
        }
        redraw();
    });

    // Was a 16 ms polling timer. The pen thread now wakes the main loop
    // directly via g_idle_add when a sample arrives, removing up to ~16 ms
    // of input latency per stroke point.
    enter_browser();
    gtk_main();

    if (markdown_dirty_ && !markdown_path_.empty())
        write_file(markdown_path_, markdown_buf_);
    if (note_.dirty) save_current();
    ocr_.stop();
    pen_.stop();
    inkfb_close();
    return 0;
}

void App::on_pen_sample(const PenSample &s) {
    bool was_empty;
    {
        std::lock_guard<std::mutex> g(q_mu_);
        was_empty = pen_queue_.empty();
        pen_queue_.push(s);
    }
    // Wake the GTK main loop immediately. g_idle_add is thread-safe in
    // GLib >= 2.32 (the Kindle ships much newer). Only schedule once per
    // batch — the main-thread handler drains everything in one pass, so
    // re-scheduling for every queued sample just churns the event loop.
    if (was_empty) {
        g_idle_add_full(G_PRIORITY_HIGH_IDLE, cb_process_now, this, nullptr);
    }
}

void App::process_async_events() {
    // While a PDF is being written we re-enter the GTK loop to animate the
    // progress bar. Don't drain pen/OCR events here — both mutate note_, which
    // export_pdf is reading. They stay queued and run once export finishes.
    if (exporting_pdf_) return;

    std::queue<PenSample> pq;
    std::queue<OcrResult> oq;
    {
        std::lock_guard<std::mutex> g(q_mu_);
        std::swap(pq, pen_queue_);
        std::swap(oq, ocr_queue_);
    }

    while (!pq.empty()) {
        const PenSample s = pq.front(); pq.pop();
        if (screen_ != Screen::NoteView) continue;
        // The dropdown is opened by finger (GTK) taps; a stylus touch on the
        // canvas dismisses it so drawing isn't blocked by a stale overlay.
        if (s.down && toolbar_.pen_menu_open()) {
            double extent = toolbar_.pen_menu_extent();
            toolbar_.close_pen_menu();
            redraw_rect(0, 0, (double)win_w_, extent);
        }
        if (s.tool == PenButton::Rubber && tool_.current != Tool::Eraser) {
            tool_.current = Tool::Eraser;
            redraw_toolbar();   // only the active-tool indicator changes
        } else if (s.tool == PenButton::Pen && tool_.current == Tool::Eraser) {
            tool_.current = Tool::Pen;
            redraw_toolbar();
        }

        double px, py;
        map_pen_to_page(s, px, py);

        if (tool_.current == Tool::Pen) {
            if (s.down && !pen_down_) {
                pen_down_ = true;
                const PenPreset &pp = kPenPresets[tool_.pen_preset];
                live_stroke_ = Stroke{};
                live_stroke_.tool     = Tool::Pen;
                live_stroke_.pen_type = pp.type;
                live_stroke_.width    = pp.width;
                // Calibration aid: dumps the raw device coords and where
                // they mapped to. Draw a dot in each corner and read these
                // off the log to derive the exact swap/invert combo.
                const PenCalibration &c = pen_.calibration();
                log_info("pen-down raw=(%d,%d) range x[%d..%d] y[%d..%d] "
                         "-> page=(%.0f,%.0f) win=%dx%d "
                         "swap=%d ix=%d iy=%d",
                         s.x, s.y, c.min_x, c.max_x, c.min_y, c.max_y,
                         px, py, win_w_, win_h_,
                         (int)c.swap_xy, (int)c.invert_x, (int)c.invert_y);
            }
            if (pen_down_ && s.down) {
                Point p; p.x = px; p.y = py;
                p.pressure = (float)s.pressure /
                    (float)std::max(1, pen_.calibration().max_pressure);
                p.t_ms = s.t_ms;

                // Page coords == screen coords now (overlay model), so no
                // toolbar offset. When the fast framebuffer path is live,
                // draw ONLY through it — the GTK partial redraw would fire a
                // second, slower e-ink refresh over the same region and
                // reintroduce lag. The clean cairo render happens once on
                // pen-up. When inkfb is unavailable, fall back to the GTK
                // partial redraw so the stroke is still visible mid-draw.
                if (!live_stroke_.pts.empty()) {
                    const Point &prev = live_stroke_.pts.back();
                    if (inkfb_available()) {
                        InkRect r = inkfb_draw_segment(prev.x, prev.y,
                                                       p.x, p.y,
                                                       live_stroke_.width);
                        if (live_ink_bbox_.w == 0) {
                            live_ink_bbox_ = r;
                        } else {
                            int ax = std::min(live_ink_bbox_.x, r.x);
                            int ay = std::min(live_ink_bbox_.y, r.y);
                            int bx = std::max(live_ink_bbox_.x + live_ink_bbox_.w,
                                              r.x + r.w);
                            int by = std::max(live_ink_bbox_.y + live_ink_bbox_.h,
                                              r.y + r.h);
                            live_ink_bbox_ = {ax, ay, bx - ax, by - ay};
                        }
                    } else {
                        double minx = std::min(prev.x, p.x) - live_stroke_.width;
                        double miny = std::min(prev.y, p.y) - live_stroke_.width;
                        double maxx = std::max(prev.x, p.x) + live_stroke_.width;
                        double maxy = std::max(prev.y, p.y) + live_stroke_.width;
                        redraw_rect(minx, miny, maxx - minx, maxy - miny);
                    }
                } else if (!inkfb_available()) {
                    redraw_rect(p.x - live_stroke_.width,
                                p.y - live_stroke_.width,
                                live_stroke_.width * 2,
                                live_stroke_.width * 2);
                }
                live_stroke_.pts.push_back(p);
            }
            if (!s.down && pen_down_) {
                pen_down_ = false;
                if (!live_stroke_.pts.empty() &&
                    current_page_ < (int)note_.pages.size()) {
                    // Bbox (page-local) of the finished stroke, before move.
                    Rect bb = live_stroke_.bbox();
                    double sw = live_stroke_.width;
                    note_.pages[current_page_].strokes.push_back(
                        std::move(live_stroke_));
                    live_stroke_ = Stroke{};
                    note_.mark_dirty();
                    if (tool_.ocr_enabled) {
                        ocr_.notify(note_, current_page_, win_w_, win_h_);
                    }
                    if (inkfb_available() && live_ink_bbox_.w > 0) {
                        // inkfb already painted this stroke into the
                        // framebuffer; a GC16 settle just snaps the A2 ghost to
                        // clean grey without altering the ink. Deliberately do
                        // NOT re-render through cairo here — cairo draws a
                        // subtly different (antialiased, pressure-tapered)
                        // stroke, which made the ink visibly "snap"/sharpen the
                        // instant the pen lifted. Leaving the as-drawn pixels
                        // means nothing changes on pen-up, like the stock app.
                        inkfb_settle(live_ink_bbox_);
                    } else {
                        // No fast framebuffer path: the live preview was GTK
                        // partial redraws, so paint the committed stroke once
                        // through cairo. Page coords == screen coords, so a
                        // partial repaint of the stroke bbox suffices (a full
                        // redraw would fire a slow whole-screen e-ink refresh).
                        redraw_rect(bb.x - sw, bb.y - sw,
                                    bb.w + 2 * sw, bb.h + 2 * sw);
                    }
                    live_ink_bbox_ = {0, 0, 0, 0};
                }
            }
        } else if (tool_.current == Tool::Eraser) {
            if (s.down && current_page_ < (int)note_.pages.size()) {
                // Sweep from the previous sample to this one so a fast drag
                // erases the whole path, not isolated dots.
                double ex0 = erase_active_ ? erase_px_ : px;
                double ey0 = erase_active_ ? erase_py_ : py;
                erase_active_ = true;
                erase_px_ = px; erase_py_ = py;
                Rect d = canvas_erase_at(note_.pages[current_page_],
                                         ex0, ey0, px, py, tool_.eraser_radius);
                if (d.w > 0 || d.h > 0) {
                    note_.mark_dirty();
                    // Repaint ONLY the erased footprint — a full redraw() here
                    // fired a slow whole-screen e-ink refresh on every sample
                    // and dropped frames mid-drag.
                    redraw_rect(d.x, d.y, d.w, d.h);
                }
            } else if (!s.down) {
                erase_active_ = false;
            }
        } else if (tool_.current == Tool::Lasso) {
            if (s.down) {
                if (!pen_down_) { pen_down_ = true; lasso_.clear(); }
                Point p; p.x = px; p.y = py;
                lasso_.pts.push_back(p);
                // Repaint only the lasso's growing bbox, not the whole screen,
                // so dragging stays responsive instead of dropping frames.
                Rect lb = lasso_.bbox();
                redraw_rect(lb.x - 4, lb.y - 4, lb.w + 8, lb.h + 8);
            } else if (pen_down_) {
                pen_down_ = false;
                lasso_.closed = lasso_.pts.size() >= 3;
                if (lasso_.closed) {
                    // Open the modal picker; user picks the target note.
                    picker_.open    = true;
                    picker_.anchor  = lasso_.bbox();
                    picker_.page    = current_page_;
                    picker_.scroll  = 0;
                    picker_.entries = index_.walk_vault();
                    lasso_.clear();
                }
                redraw();
            }
        }
    }

    while (!oq.empty()) {
        OcrResult r = std::move(oq.front()); oq.pop();
        if ((int)note_.ocr_text.size() <= r.page)
            note_.ocr_text.resize(r.page + 1);
        note_.ocr_text[r.page] = r.text;
        for (size_t i = 0; i < r.wiki_links.size(); ++i) {
            const auto &name = r.wiki_links[i];
            std::string resolved = index_.resolve_link(name);
            if (resolved.empty()) {
                // Brand-new link target → create an .md in the current
                // browser folder (Obsidian default behaviour).
                index_.create_markdown(name);
            }
            Link l;
            l.page   = r.page;
            l.target = name;  // store the wiki-style name; resolved at follow time
            if (i < r.word_rects.size() && r.word_rects[i].w > 0) {
                // Tesseract gives page-image coords (0 = top of page),
                // which is exactly our page-local space — store as-is.
                l.rect = r.word_rects[i];
            } else {
                // Placeholder when we don't have a word bbox.
                l.rect = {40, 40 + (double)i * 32, 220, 28};
            }
            note_.links.push_back(l);
            note_.mark_dirty();
        }
        if (!r.wiki_links.empty()) {
            status_ = "OCR linked [[..]]";
            redraw();
        }
    }
}

void App::map_pen_to_page(const PenSample &s, double &px, double &py) {
    const PenCalibration &c = pen_.calibration();
    double nx = (double)(s.x - c.min_x) / (double)std::max(1, c.max_x - c.min_x);
    double ny = (double)(s.y - c.min_y) / (double)std::max(1, c.max_y - c.min_y);
    if (c.invert_x) nx = 1.0 - nx;
    if (c.invert_y) ny = 1.0 - ny;
    if (c.swap_xy) std::swap(nx, ny);
    // Overlay model: the page fills the whole screen and the toolbar is
    // drawn on top of it, so page coords == screen coords. The pen's full
    // physical range maps straight to the full window — ink lands exactly
    // under the pen, and hiding/showing the toolbar never shifts strokes.
    px = nx * (double)win_w_;
    py = ny * (double)win_h_;
}

void App::enter_browser() {
    if (note_.dirty) save_current();
    if (markdown_dirty_ && !markdown_path_.empty()) {
        write_file(markdown_path_, markdown_buf_);
        markdown_dirty_ = false;
    }
    screen_ = Screen::Browser;
    index_.open(notes_dir_, browser_path_);
    browser_.set_current_path(browser_path_);
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
    std::string dir = notes_dir_ + "/" + id;
    Note n;
    if (!load_note(dir, n)) {
        log_err("failed to load %s", dir.c_str());
        return;
    }
    note_ = std::move(n);
    // Canonicalise: the id stored in-memory is always the vault-relative
    // path, regardless of what was on disk. enter_note(id) below + the
    // save_current path concatenate notes_dir_ + "/" + note_.id, so they
    // both need this form.
    note_.id = id;
    current_page_ = 0;
    history_.push({note_.id, current_page_});
    screen_ = Screen::NoteView;
    redraw();
}

void App::enter_markdown(const std::string &path) {
    if (markdown_dirty_ && !markdown_path_.empty())
        write_file(markdown_path_, markdown_buf_);
    markdown_path_ = path;
    markdown_buf_.clear();
    read_file(path, markdown_buf_);
    markdown_cursor_ = markdown_buf_.size();
    markdown_history_.clear();
    markdown_dirty_ = false;
    screen_ = Screen::Markdown;
    redraw();
}

void App::markdown_snapshot() {
    // Cap history to keep memory bounded; ~200 line-boundary snapshots
    // is plenty even for long sessions.
    if (markdown_history_.size() > 200) {
        markdown_history_.erase(markdown_history_.begin());
    }
    markdown_history_.emplace_back(markdown_buf_, markdown_cursor_);
}

void App::markdown_undo() {
    if (markdown_history_.empty()) return;
    auto [buf, cur] = markdown_history_.back();
    markdown_history_.pop_back();
    markdown_buf_    = std::move(buf);
    markdown_cursor_ = std::min(cur, markdown_buf_.size());
    markdown_dirty_  = true;
    redraw();
}

void App::markdown_insert(const std::string &text) {
    if (markdown_cursor_ > markdown_buf_.size()) markdown_cursor_ = markdown_buf_.size();
    markdown_buf_.insert(markdown_cursor_, text);
    markdown_cursor_ += text.size();
    markdown_dirty_ = true;
}

void App::markdown_backspace() {
    if (markdown_cursor_ == 0 || markdown_buf_.empty()) return;
    // Skip back one UTF-8 codepoint (treat continuation bytes 10xxxxxx).
    size_t n = 1;
    while (n < markdown_cursor_ &&
           (((unsigned char)markdown_buf_[markdown_cursor_ - n]) & 0xC0) == 0x80) {
        ++n;
    }
    markdown_buf_.erase(markdown_cursor_ - n, n);
    markdown_cursor_ -= n;
    markdown_dirty_ = true;
}

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
    std::string out = notes_dir_ + "/" + note_.id + ".pdf";
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
    // off (see on_button_*/process_async_events) while exporting_pdf_ is set,
    // so the re-entrant pump can't mutate the note mid-write.
    exporting_pdf_ = true;
    export_total_  = (int)std::max<size_t>(1, note_.pages.size());
    export_done_   = 0;
    status_        = "exporting pdf…";
    redraw();   // lay down the dim backdrop + initial bar
    while (gtk_events_pending()) gtk_main_iteration();

    bool ok = export_pdf(out, note_, page_w, page_h, src_w, src_h,
        [this](int done, int total) {
            export_done_  = done;
            export_total_ = std::max(1, total);
            Rect c = export_overlay_rect();
            redraw_rect(c.x, c.y, c.w, c.h);   // update just the card
            while (gtk_events_pending()) gtk_main_iteration();
        });

    exporting_pdf_ = false;
    status_ = ok ? ("pdf → " + out) : "pdf failed";
    redraw();   // clear the overlay
}

Rect App::export_overlay_rect() const {
    double cw = std::min(460.0, win_w_ * 0.8);
    double ch = 150.0;
    double cx = (win_w_ - cw) / 2.0;
    double cy = (win_h_ - ch) / 2.0;
    return Rect{cx, cy, cw, ch};
}

void App::draw_export_overlay(cairo_t *cr, int win_w, int win_h) {
    // Dim the page behind the card.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    Rect c = export_overlay_rect();
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_stroke(cr);

    // Title.
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 16");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Exporting PDF  %d/%d",
                  std::min(export_done_, export_total_), export_total_);
    pango_layout_set_text(layout, buf, -1);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_move_to(cr, c.x + 24, c.y + 26);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    // Progress bar track + fill.
    double bx = c.x + 24, bw = c.w - 48;
    double by = c.y + c.h - 48, bh = 22;
    double frac = export_total_ > 0
        ? (double)export_done_ / (double)export_total_ : 0.0;
    if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_rectangle(cr, bx, by, bw, bh); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, bx, by, bw * frac, bh); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bx, by, bw, bh); cairo_stroke(cr);
}

// --- Browser modals (rename input / delete confirm) ---

Rect App::modal_card_rect() const {
    double cw = std::min(560.0, win_w_ * 0.8);
    double ch = 240.0;
    double cx = (win_w_ - cw) / 2.0;
    double cy = 160.0;   // near the top, leaving room for the keyboard below
    return Rect{cx, cy, cw, ch};
}

Rect App::modal_btn_rect(bool primary) const {
    Rect c = modal_card_rect();
    double bw = 150, bh = 60, gap = 20, m = 24;
    double by = c.y + c.h - bh - m;
    double px = primary ? (c.x + c.w - m - bw)             // right: Save/Delete
                        : (c.x + c.w - m - 2 * bw - gap);  // left:  Cancel
    return Rect{px, by, bw, bh};
}

void App::open_rename(const std::string &id, const std::string &cur_title) {
    input_open_      = true;
    input_title_     = "Rename";
    input_buf_       = cur_title;
    input_target_id_ = id;
    keyboard_.set_visible(true);
    redraw();
}

void App::commit_input() {
    std::string id = input_target_id_, name = input_buf_;
    close_input();
    if (!id.empty() && !name.empty()) index_.rename_entry(id, name);
    enter_browser();   // rescans the dir + redraws
}

void App::close_input() {
    input_open_ = false;
    input_buf_.clear();
    input_target_id_.clear();
    keyboard_.set_visible(false);
    redraw();
}

namespace {
void modal_button(cairo_t *cr, const Rect &r, const char *label, bool primary) {
    cairo_set_source_rgb(cr, primary ? 0.20 : 0.90,
                             primary ? 0.20 : 0.90,
                             primary ? 0.20 : 0.90);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_stroke(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 18");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, label, -1);
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    double fg = primary ? 0.95 : 0.12;
    cairo_set_source_rgb(cr, fg, fg, fg);
    cairo_move_to(cr, r.x + (r.w - tw) / 2.0, r.y + (r.h - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}
}  // namespace

void App::draw_input_modal(cairo_t *cr, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    Rect c = modal_card_rect();
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_stroke(cr);

    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 22");
    PangoLayout *t = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(t, fd);
    pango_layout_set_text(t, input_title_.c_str(), -1);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, c.x + 24, c.y + 18);
    pango_cairo_show_layout(cr, t);
    g_object_unref(t);
    pango_font_description_free(fd);

    // Editable text field with a trailing cursor bar.
    Rect f{c.x + 24, c.y + 74, c.w - 48, 56};
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, f.x, f.y, f.w, f.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, f.x, f.y, f.w, f.h); cairo_stroke(cr);

    std::string shown = input_buf_ + "|";
    PangoFontDescription *ffd = pango_font_description_from_string("Sans 20");
    PangoLayout *fl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(fl, ffd);
    pango_layout_set_text(fl, shown.c_str(), -1);
    int tw, th; pango_layout_get_pixel_size(fl, &tw, &th);
    (void)tw;
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, f.x + 12, f.y + (f.h - th) / 2.0);
    pango_cairo_show_layout(cr, fl);
    g_object_unref(fl);
    pango_font_description_free(ffd);

    modal_button(cr, modal_btn_rect(true),  "Save",   true);
    modal_button(cr, modal_btn_rect(false), "Cancel", false);
}

void App::draw_confirm_modal(cairo_t *cr, int win_w, int win_h) {
    // Dim the screen behind the confirm card.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, 0, 0, win_w, win_h); cairo_fill(cr);

    Rect c = modal_card_rect();
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h); cairo_stroke(cr);

    PangoFontDescription *fd = pango_font_description_from_string("Sans 22");
    PangoLayout *t = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(t, fd);
    pango_layout_set_width(t, (int)((c.w - 48) * PANGO_SCALE));
    pango_layout_set_text(t, confirm_text_.c_str(), -1);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, c.x + 24, c.y + 36);
    pango_cairo_show_layout(cr, t);
    g_object_unref(t);
    pango_font_description_free(fd);

    modal_button(cr, modal_btn_rect(true),  "Delete", true);
    modal_button(cr, modal_btn_rect(false), "Cancel", false);
}

void App::set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) deg = 0;
    rotation_ = deg;
    redraw();
}

void App::screen_to_drawing(double sx, double sy, double &dx, double &dy) const {
    // Inverse of the cairo transform applied in on_draw(). xw_/xh_ are
    // the X11 window dims; output dx/dy is in drawing (portrait) space.
    switch (rotation_) {
    case 90:  dx = sy;                  dy = (double)xw_ - sx;       break;
    case 180: dx = (double)xw_ - sx;    dy = (double)xh_ - sy;       break;
    case 270: dx = (double)xh_ - sy;    dy = sx;                     break;
    default:  dx = sx;                  dy = sy;                     break;
    }
}

void App::on_draw(cairo_t *cr, int win_w, int win_h) {
    xw_ = win_w; xh_ = win_h;
    win_w_ = draw_w(); win_h_ = draw_h();

    // Software-rotate the cairo context so the rest of the renderer can
    // assume portrait coordinates. The Kindle X server doesn't expose
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

    // From here on, use the drawing-space dims (portrait).
    win_w = win_w_; win_h = win_h_;

    if (screen_ == Screen::Browser) {
        browser_.layout(win_w, win_h, index_.entries().size());
        browser_.draw(cr, index_, win_w, win_h);
        // Rename input modal (with on-screen keyboard) / delete confirm sit
        // on top of the browser.
        if (input_open_) {
            keyboard_.layout(win_w, win_h);
            keyboard_.draw(cr);
            draw_input_modal(cr, win_w, win_h);
        }
        if (confirm_open_) draw_confirm_modal(cr, win_w, win_h);
        return;
    }

    toolbar_.layout(win_w);
    if (screen_ == Screen::NoteView) {
        // Full-screen page; the toolbar overlays the top when visible.
        double page_w = win_w;
        double page_h = win_h;
        // Render strokes WITHOUT antialiasing so the committed/redrawn ink
        // matches the hard-edged live ink the inkfb fast path draws. With AA
        // on, strokes looked jagged while drawing then "smoothed out" on the
        // next cairo redraw — a visible, jarring change. (PDF export uses its
        // own context and keeps AA, so exports stay smooth.) Restored to the
        // default before the rest of the UI so text/icons still antialias.
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
        if (current_page_ < (int)note_.pages.size())
            canvas_render_page(cr, note_.pages[current_page_], page_w, page_h);
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
    } else if (screen_ == Screen::Markdown) {
        // Splice a visible cursor marker into the buffer at cursor pos so
        // the user can see where typing will land. U+2502 BOX DRAWINGS
        // LIGHT VERTICAL renders as a thin vertical bar in monospace and
        // most prose fonts; it's safe to embed in Pango markup.
        std::string with_cursor = markdown_buf_;
        size_t cur = std::min(markdown_cursor_, with_cursor.size());
        with_cursor.insert(cur, "\xe2\x94\x82");  // "│"
        double top = toolbar_.height() + 12;
        double y = render_markdown(cr, 24, top, win_w - 48, with_cursor);
        (void)y;
    }

    if (keyboard_.visible()) {
        keyboard_.layout(win_w, win_h);
        keyboard_.draw(cr);
    }

    // Floating "show toolbar" tab whenever the toolbar is hidden.
    if (screen_ != Screen::Browser && !toolbar_.visible()) {
        draw_show_tab(cr, win_w);
    }

    toolbar_.draw(cr, tool_, status_, current_page_,
                  (int)std::max<size_t>(1, note_.pages.size()));

    // Link picker modal sits on top of everything else.
    if (picker_.open) {
        draw_link_picker(cr, win_w, win_h);
    }

    // PDF export progress sits above even the picker.
    if (exporting_pdf_) {
        draw_export_overlay(cr, win_w, win_h);
    }
}

void App::handle_picker_press(double x, double y) {
    // Recompute layout to match draw_link_picker exactly. Could be
    // factored out, but keeping it inline avoids state shared between
    // draw and hit-test.
    int win_w = win_w_, win_h = win_h_;
    double cw = std::min(640.0, win_w * 0.85);
    double ch = std::min(960.0, win_h * 0.80);
    double cx = (win_w - cw) / 2.0;
    double cy = (win_h - ch) / 2.0;

    // Close (X)
    double xs = 56;
    Rect close_r{cx + cw - xs - 12, cy + 12, xs, xs};
    if (close_r.contains(x, y)) {
        picker_.open = false;
        redraw();
        return;
    }

    // Tap outside the card cancels too.
    if (x < cx || x > cx + cw || y < cy || y > cy + ch) {
        picker_.open = false;
        redraw();
        return;
    }

    double row_h = 64;
    double list_top = cy + 80;
    double list_bot = cy + ch - 16;
    int rows_visible = std::max(0, (int)((list_bot - list_top) / row_h));

    // Scroll zones — top/bottom 40px of the list area
    if (y >= list_top && y < list_top + 40 && picker_.scroll > 0) {
        picker_.scroll = std::max(0, picker_.scroll - rows_visible);
        redraw();
        return;
    }
    if (y >= list_bot - 40 && y <= list_bot &&
        picker_.scroll + rows_visible < (int)picker_.entries.size()) {
        picker_.scroll = std::min((int)picker_.entries.size() - 1,
                                   picker_.scroll + rows_visible);
        redraw();
        return;
    }

    if (y >= list_top && y < list_bot) {
        int row = (int)((y - list_top) / row_h);
        int idx = picker_.scroll + row;
        if (idx >= 0 && idx < (int)picker_.entries.size()) {
            const auto &e = picker_.entries[idx];
            Link l;
            l.page   = picker_.page;
            l.rect   = picker_.anchor;
            // Store as wiki-link name (basename without .md). resolve_link
            // does fuzzy matching at follow time.
            std::string name = e.id;
            auto slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (name.size() > 3 &&
                name.compare(name.size() - 3, 3, ".md") == 0)
                name.resize(name.size() - 3);
            l.target = name;
            note_.links.push_back(l);
            note_.mark_dirty();
            status_ = "linked → " + e.title;
            picker_.open = false;
            redraw();
        }
    }
}

void App::draw_link_picker(cairo_t *cr, int win_w, int win_h) {
    // Dim backdrop
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    // Card centered on screen
    double cw = std::min(640.0, win_w * 0.85);
    double ch = std::min(960.0, win_h * 0.80);
    double cx = (win_w - cw) / 2.0;
    double cy = (win_h - ch) / 2.0;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, cx, cy, cw, ch); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, cx, cy, cw, ch); cairo_stroke(cr);

    // Header
    PangoLayout *h = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 24");
    pango_layout_set_font_description(h, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(h, "Link target", -1);
    cairo_move_to(cr, cx + 24, cy + 18);
    pango_cairo_show_layout(cr, h);
    g_object_unref(h);

    // Close (X) button
    double xs = 56;
    Rect close_r{cx + cw - xs - 12, cy + 12, xs, xs};
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_rectangle(cr, close_r.x, close_r.y, close_r.w, close_r.h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, close_r.x, close_r.y, close_r.w, close_r.h);
    cairo_stroke(cr);
    PangoLayout *xl = pango_cairo_create_layout(cr);
    fd = pango_font_description_from_string("Sans Bold 22");
    pango_layout_set_font_description(xl, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(xl, "X", -1);
    cairo_move_to(cr, close_r.x + 18, close_r.y + 12);
    pango_cairo_show_layout(cr, xl);
    g_object_unref(xl);

    // Entry rows
    double row_h = 64;
    double list_top = cy + 80;
    double list_bot = cy + ch - 16;
    int rows_visible = std::max(0, (int)((list_bot - list_top) / row_h));
    auto &es = picker_.entries;
    for (int i = 0; i < rows_visible; ++i) {
        int idx = picker_.scroll + i;
        if (idx < 0 || idx >= (int)es.size()) break;
        double ry = list_top + i * row_h;
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, cx + 16, ry + row_h);
        cairo_line_to(cr, cx + cw - 16, ry + row_h);
        cairo_stroke(cr);

        PangoLayout *t = pango_cairo_create_layout(cr);
        fd = pango_font_description_from_string("Sans 18");
        pango_layout_set_font_description(t, fd);
        pango_font_description_free(fd);
        std::string label = es[idx].id;  // shows folder/file.md
        if (es[idx].is_markdown) label += "  [MD]";
        pango_layout_set_text(t, label.c_str(), -1);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr, cx + 24, ry + 16);
        pango_cairo_show_layout(cr, t);
        g_object_unref(t);
    }

    // Scroll hints
    if (picker_.scroll > 0) {
        PangoLayout *t = pango_cairo_create_layout(cr);
        fd = pango_font_description_from_string("Sans 14");
        pango_layout_set_font_description(t, fd);
        pango_font_description_free(fd);
        pango_layout_set_text(t, "▲ tap top edge to scroll up", -1);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, cx + 24, cy + 60);
        pango_cairo_show_layout(cr, t);
        g_object_unref(t);
    }
    if (picker_.scroll + rows_visible < (int)es.size()) {
        PangoLayout *t = pango_cairo_create_layout(cr);
        fd = pango_font_description_from_string("Sans 14");
        pango_layout_set_font_description(t, fd);
        pango_font_description_free(fd);
        pango_layout_set_text(t, "▼ tap bottom edge to scroll down", -1);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, cx + 24, cy + ch - 30);
        pango_cairo_show_layout(cr, t);
        g_object_unref(t);
    }
}

bool App::on_button_press(double x, double y) {
    // PDF export is a blocking modal; ignore input until it finishes.
    if (exporting_pdf_) return true;

    // Modal: link picker intercepts everything else.
    if (picker_.open) {
        handle_picker_press(x, y);
        return true;
    }

    // Pen-preset dropdown swallows presses; the matching release selects a
    // preset or dismisses the menu.
    if (toolbar_.pen_menu_open()) return true;

    if (screen_ == Screen::Browser) {
        // Modals are acted on release; here we just swallow the press (and
        // give the keyboard its press feedback when the rename modal is open).
        if (confirm_open_) return true;
        if (input_open_) {
            if (keyboard_.visible() && y >= keyboard_.top_y()) {
                keyboard_.press(x, y);
                redraw_rect(0, keyboard_.top_y(),
                            (double)win_w_, (double)win_h_ - keyboard_.top_y());
            }
            return true;
        }
        BrowserHit h = browser_.hit(x, y, index_);
        switch (h.action) {
            case BrowserAction::NewNote: {
                auto e = index_.create_note("Untitled", TemplateId::Blank);
                enter_note(e.id);
                break;
            }
            case BrowserAction::NewMarkdown: {
                auto e = index_.create_markdown("untitled");
                enter_markdown(e.path);
                break;
            }
            case BrowserAction::OpenParent:
                enter_parent_folder();
                break;
            case BrowserAction::Open:
                if (h.entry_index >= 0) {
                    const auto &e = index_.entries()[h.entry_index];
                    if (e.is_folder)        enter_folder(e.id);
                    else if (e.is_markdown) enter_markdown(e.path);
                    else                    enter_note(e.id);
                }
                break;
            case BrowserAction::Rename:
                if (h.entry_index >= 0) {
                    const auto &e = index_.entries()[h.entry_index];
                    open_rename(e.id, e.title);
                }
                break;
            case BrowserAction::Delete:
                if (h.entry_index >= 0) {
                    const auto &e = index_.entries()[h.entry_index];
                    confirm_open_      = true;
                    confirm_target_id_ = e.id;
                    confirm_text_      = "Delete \"" + e.title + "\"?";
                    redraw();
                }
                break;
            default: break;
        }
        return true;
    }

    // When the toolbar is hidden, consume a press on the "show" tab here;
    // the actual show happens on release so the release doesn't land on a
    // freshly-revealed toolbar button (which sits at the same top-right spot
    // and would immediately hide it again).
    if (screen_ != Screen::Browser && !toolbar_.visible()) {
        if (show_tab_rect().contains(x, y)) {
            return true;
        }
        // Taps elsewhere fall through to normal handling (link follow, etc.).
    }

    if (y < toolbar_.height()) {
        toolbar_.press(x, y);
        // Repaint only the tapped button — a much smaller e-ink refresh than
        // the whole strip, so press feedback feels immediate.
        Rect br = toolbar_.button_rect_at(x, y);
        if (br.w > 0) redraw_rect(br.x, br.y, br.w, br.h);
        else          redraw_toolbar();
        return true;
    }
    if (keyboard_.visible() && y >= keyboard_.top_y()) {
        keyboard_.press(x, y);
        // Key-press feedback only repaints the on-screen keyboard, not the
        // whole markdown body above it.
        redraw_rect(0, keyboard_.top_y(),
                    (double)win_w_, (double)win_h_ - keyboard_.top_y());
        return true;
    }

    // NoteView canvas: begin a potential finger swipe for page navigation.
    // The stylus draws through the evdev path, so these GTK pointer events are
    // finger input. Swipe-vs-tap (tap = follow a link) is decided on release.
    if (screen_ == Screen::NoteView) {
        swipe_active_ = true;
        swipe_x0_ = x;
        swipe_y0_ = y;
    }
    return true;
}

bool App::on_button_release(double x, double y) {
    if (exporting_pdf_) return true;

    if (screen_ == Screen::Browser) {
        if (confirm_open_) {
            if (modal_btn_rect(true).contains(x, y)) {          // Delete
                index_.remove_entry(confirm_target_id_);
                confirm_open_ = false;
                enter_browser();                                // rescan + redraw
            } else {                                            // Cancel / outside
                confirm_open_ = false;
                redraw();
            }
            return true;
        }
        if (input_open_) {
            if (keyboard_.visible() && y >= keyboard_.top_y()) {
                keyboard_.release(x, y);   // dispatches via on_text/on_key
                redraw();
                return true;
            }
            if (modal_btn_rect(true).contains(x, y))       commit_input();  // Save
            else if (modal_btn_rect(false).contains(x, y)) close_input();   // Cancel
            return true;
        }
        return true;
    }

    // NoteView finger gesture: a horizontal swipe flips the page, a tap
    // follows a link. swipe_active_ is only set for canvas presses, so this
    // can't be confused with toolbar/keyboard taps.
    if (screen_ == Screen::NoteView && swipe_active_) {
        swipe_active_ = false;
        double dx = x - swipe_x0_, dy = y - swipe_y0_;
        const double SWIPE_MIN = 120.0;
        if (std::fabs(dx) > SWIPE_MIN && std::fabs(dx) > 2.0 * std::fabs(dy)) {
            int np = pages_clamp(note_, current_page_ + (dx < 0 ? 1 : -1));
            if (np != current_page_) { current_page_ = np; redraw(); }
        } else if (tool_.current == Tool::Pen) {
            const Link *l = link_at(note_, current_page_, x, y);
            if (l) {
                std::string resolved = index_.resolve_link(l->target);
                struct stat st;
                if (!resolved.empty() && stat(resolved.c_str(), &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        std::string rel = resolved;
                        if (rel.rfind(notes_dir_ + "/", 0) == 0)
                            rel = rel.substr(notes_dir_.size() + 1);
                        enter_note(rel);
                    } else {
                        enter_markdown(resolved);
                    }
                }
            }
        }
        return true;
    }

    // Toolbar hidden: the only chrome is the show-tab. Reveal it here on
    // release (the toolbar is still hidden at this point, so this release
    // can't accidentally trigger a toolbar button).
    if (!toolbar_.visible()) {
        if (show_tab_rect().contains(x, y)) {
            toolbar_.set_visible(true);
            redraw_toolbar();   // only the top strip changes — fast refresh
        }
        return true;
    }

    // Pen-preset dropdown is modal-ish: a tap either picks a preset or
    // dismisses it. Its items hang below the toolbar strip, so this must run
    // before the y < height() gate.
    if (toolbar_.pen_menu_open()) {
        int preset = toolbar_.pen_menu_hit(x, y);
        if (preset >= 0) {
            tool_.current    = Tool::Pen;
            tool_.pen_preset = preset;
        }
        double extent = toolbar_.pen_menu_extent();
        toolbar_.close_pen_menu();
        redraw_rect(0, 0, (double)win_w_, extent);   // clear just the band
        return true;
    }

    if (y < toolbar_.height()) {
        ToolbarAction a = toolbar_.hit(x, y);
        toolbar_.release_all();
        // Most actions only change the toolbar/status; those get a fast
        // partial refresh. Actions that change the page/screen content set
        // full_redraw so the whole canvas repaints.
        bool full_redraw = false;
        switch (a) {
            case ToolbarAction::Pen:
                tool_.current = Tool::Pen;
                toolbar_.toggle_pen_menu();   // reveal/hide the preset dropdown
                // Repaint just the toolbar strip + dropdown band — a full
                // redraw fires a slow whole-screen e-ink refresh.
                redraw_rect(0, 0, (double)win_w_, toolbar_.pen_menu_extent());
                return true;
            case ToolbarAction::Eraser:    tool_.current = Tool::Eraser; break;
            case ToolbarAction::Lasso:     tool_.current = Tool::Lasso;  break;
            case ToolbarAction::Keyboard:
                tool_.keyboard_visible = !tool_.keyboard_visible;
                keyboard_.set_visible(tool_.keyboard_visible);
                full_redraw = true;   // keyboard shows/hides a big region
                break;
            case ToolbarAction::OcrToggle:
                tool_.ocr_enabled = !tool_.ocr_enabled;
                ocr_.set_enabled(tool_.ocr_enabled);
                break;
            case ToolbarAction::Save:      save_current(); break;
            case ToolbarAction::ExportPdf: export_current_pdf(); break;
            case ToolbarAction::AddPage:
                if (screen_ == Screen::NoteView) {
                    pages_append(note_, note_.default_template);
                    current_page_ = (int)note_.pages.size() - 1;
                    note_.mark_dirty();
                }
                full_redraw = true;
                break;
            case ToolbarAction::PrevPage:
                current_page_ = pages_clamp(note_, current_page_ - 1);
                full_redraw = true;
                break;
            case ToolbarAction::NextPage:
                current_page_ = pages_clamp(note_, current_page_ + 1);
                full_redraw = true;
                break;
            case ToolbarAction::Back:
                if (history_.can_back()) {
                    auto e = history_.pop_back_to_prev();
                    enter_note(e.note_id);
                    current_page_ = e.page;
                } else {
                    enter_browser();
                }
                full_redraw = true;
                break;
            case ToolbarAction::Browser: enter_browser(); full_redraw = true; break;
            case ToolbarAction::Undo:
                if (screen_ == Screen::Markdown) markdown_undo();
                full_redraw = true;   // markdown body changed
                break;
            case ToolbarAction::Hide:
                toolbar_.set_visible(false);
                // Only the toolbar strip changes (toolbar → page content),
                // so leave full_redraw=false → fast partial refresh.
                break;
            case ToolbarAction::Exit:
                // Persist outstanding work before tearing down. The rest of
                // the shutdown (ocr/pen/inkfb stop) runs in run()'s epilogue
                // after gtk_main() returns.
                if (markdown_dirty_ && !markdown_path_.empty()) {
                    write_file(markdown_path_, markdown_buf_);
                    markdown_dirty_ = false;
                }
                if (note_.dirty) save_current();
                gtk_main_quit();
                return true;
            default: break;
        }
        if (full_redraw) redraw();
        else             redraw_toolbar();
        return true;
    }
    if (keyboard_.visible() && y >= keyboard_.top_y()) {
        keyboard_.release(x, y);
        redraw();
        return true;
    }
    return true;
}

bool App::on_motion(double, double) {
    // Pen-driven motion comes via the evdev thread, not GTK.
    return true;
}

} // namespace bn
