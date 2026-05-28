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

    // Direct-to-framebuffer ink path. Skipped if BN_DISABLE_INKFB=1 set
    // (useful for testing on hosts where /dev/fb0 isn't the e-ink panel).
    if (!std::getenv("BN_DISABLE_INKFB")) {
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
        if (screen_ == Screen::Markdown) {
            markdown_insert(t);
            redraw();
        }
    });
    keyboard_.on_key([this](const std::string &k){
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
        if (s.tool == PenButton::Rubber && tool_.current != Tool::Eraser) {
            tool_.current = Tool::Eraser;
            redraw();
        } else if (s.tool == PenButton::Pen && tool_.current == Tool::Eraser) {
            tool_.current = Tool::Pen;
            redraw();
        }

        double px, py;
        map_pen_to_page(s, px, py);

        if (tool_.current == Tool::Pen) {
            if (s.down && !pen_down_) {
                pen_down_ = true;
                live_stroke_ = Stroke{};
                live_stroke_.tool = Tool::Pen;
                live_stroke_.width = tool_.pen_width;
            }
            if (pen_down_ && s.down) {
                Point p; p.x = px; p.y = py;
                p.pressure = (float)s.pressure /
                    (float)std::max(1, pen_.calibration().max_pressure);
                p.t_ms = s.t_ms;

                // Two paths:
                //   * inkfb_available(): write the segment straight to the
                //     e-ink framebuffer with the A2 waveform — ~30 ms ghost
                //     instead of ~150-500 ms through X11. The final pretty
                //     render still happens via cairo on pen-up.
                //   * fallback: queue a partial redraw of the new segment.
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
                    note_.pages[current_page_].strokes.push_back(
                        std::move(live_stroke_));
                    live_stroke_ = Stroke{};
                    note_.mark_dirty();
                    if (tool_.ocr_enabled) {
                        ocr_.notify(note_, current_page_, win_w_,
                                    win_h_ - (int)toolbar_.height() - 36);
                    }
                    // Snap the A2 ghost from the fast-path into a clean
                    // GC16 grey, then let GTK rerender the whole region
                    // via cairo so the stroke matches saved-state pixels.
                    if (inkfb_available() && live_ink_bbox_.w > 0) {
                        inkfb_settle(live_ink_bbox_);
                    }
                    live_ink_bbox_ = {0, 0, 0, 0};
                    redraw();
                }
            }
        } else if (tool_.current == Tool::Eraser) {
            if (s.down && current_page_ < (int)note_.pages.size()) {
                int n = canvas_erase_at(note_.pages[current_page_],
                                        px, py, tool_.eraser_radius);
                if (n > 0) { note_.mark_dirty(); redraw(); }
            }
        } else if (tool_.current == Tool::Lasso) {
            if (s.down) {
                if (!pen_down_) { pen_down_ = true; lasso_.clear(); }
                Point p; p.x = px; p.y = py;
                lasso_.pts.push_back(p);
                redraw();
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
                // Tesseract gives page-image coords (0 = top of page).
                // Link rects are stored in window-relative coords (0 =
                // top of window), so shift by the toolbar's height.
                l.rect = r.word_rects[i];
                l.rect.y += toolbar_.height();
            } else {
                // Placeholder when we don't have a word bbox.
                l.rect = {40, toolbar_.height() + 40 + (double)i * 32, 220, 28};
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
    px = nx * (double)win_w_;
    py = toolbar_.height() + ny *
        (double)std::max(0, win_h_ - (int)toolbar_.height() - 36);
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
    // A4 in points: 595 x 842
    if (export_pdf(out, note_, 595, 842))
        status_ = "pdf → " + out;
    else
        status_ = "pdf failed";
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
        return;
    }

    toolbar_.layout(win_w);
    if (screen_ == Screen::NoteView) {
        cairo_save(cr);
        cairo_translate(cr, 0, toolbar_.height());
        double page_w = win_w;
        double page_h = win_h - toolbar_.height() - 36.0;
        if (current_page_ < (int)note_.pages.size())
            canvas_render_page(cr, note_.pages[current_page_], page_w, page_h);
        if (pen_down_ && tool_.current == Tool::Pen)
            canvas_render_stroke(cr, live_stroke_);
        // Lasso preview
        if (tool_.current == Tool::Lasso && lasso_.pts.size() > 1) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
            cairo_set_line_width(cr, 1.0);
            static const double dashes[] = {4, 3};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_move_to(cr, lasso_.pts[0].x, lasso_.pts[0].y - toolbar_.height());
            for (size_t i = 1; i < lasso_.pts.size(); ++i)
                cairo_line_to(cr, lasso_.pts[i].x,
                                  lasso_.pts[i].y - toolbar_.height());
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
        }
        // Link overlays
        for (auto &l : note_.links) {
            if (l.page != current_page_) continue;
            cairo_set_source_rgba(cr, 0, 0, 1, 0.05);
            cairo_rectangle(cr, l.rect.x, l.rect.y - toolbar_.height(),
                            l.rect.w, l.rect.h);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 1, 0.4);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    } else if (screen_ == Screen::Markdown) {
        // Splice a visible cursor marker into the buffer at cursor pos so
        // the user can see where typing will land. U+2502 BOX DRAWINGS
        // LIGHT VERTICAL renders as a thin vertical bar in monospace and
        // most prose fonts; it's safe to embed in Pango markup.
        std::string with_cursor = markdown_buf_;
        size_t cur = std::min(markdown_cursor_, with_cursor.size());
        with_cursor.insert(cur, "\xe2\x94\x82");  // "│"
        double y = render_markdown(cr, 24, toolbar_.height() + 12,
                                   win_w - 48, with_cursor);
        (void)y;
    }

    if (keyboard_.visible()) {
        keyboard_.layout(win_w, win_h);
        keyboard_.draw(cr);
    }

    toolbar_.draw(cr, tool_, status_, current_page_,
                  (int)std::max<size_t>(1, note_.pages.size()));

    // Link picker modal sits on top of everything else.
    if (picker_.open) {
        draw_link_picker(cr, win_w, win_h);
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
    // Modal: link picker intercepts everything else.
    if (picker_.open) {
        handle_picker_press(x, y);
        return true;
    }

    if (screen_ == Screen::Browser) {
        BrowserHit h = browser_.hit(x, y, index_.entries().size());
        if (h.action == BrowserAction::NewNote) {
            auto e = index_.create_note("Untitled", TemplateId::Blank);
            enter_note(e.id);
        } else if (h.action == BrowserAction::NewMarkdown) {
            auto e = index_.create_markdown("untitled");
            enter_markdown(e.path);
        } else if (h.action == BrowserAction::OpenParent) {
            enter_parent_folder();
        } else if (h.action == BrowserAction::Open && h.entry_index >= 0) {
            const auto &e = index_.entries()[h.entry_index];
            if (e.is_folder)        enter_folder(e.id);
            else if (e.is_markdown) enter_markdown(e.path);
            else                    enter_note(e.id);
        }
        return true;
    }

    if (y < toolbar_.height()) {
        toolbar_.press(x, y);
        redraw();
        return true;
    }
    if (keyboard_.visible() && y >= keyboard_.top_y()) {
        keyboard_.press(x, y);
        redraw();
        return true;
    }

    // Tap-to-follow inside a link rect (Pen tool, single tap)
    if (screen_ == Screen::NoteView && tool_.current == Tool::Pen) {
        const Link *l = link_at(note_, current_page_, x, y);
        if (l) {
            // Resolve against the vault — handles bare names ("MyNote"),
            // sub-folder paths ("work/meeting"), .md vs note-dir, and
            // case/slug fuzzy matching like Obsidian.
            std::string resolved = index_.resolve_link(l->target);
            if (!resolved.empty()) {
                struct stat st;
                if (stat(resolved.c_str(), &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        // Native note dir — derive vault-relative id.
                        std::string rel = resolved;
                        if (rel.rfind(notes_dir_ + "/", 0) == 0)
                            rel = rel.substr(notes_dir_.size() + 1);
                        enter_note(rel);
                    } else {
                        enter_markdown(resolved);
                    }
                }
            }
            return true;
        }
    }
    return true;
}

bool App::on_button_release(double x, double y) {
    if (screen_ == Screen::Browser) return true;
    if (y < toolbar_.height()) {
        ToolbarAction a = toolbar_.hit(x, y);
        toolbar_.release_all();
        switch (a) {
            case ToolbarAction::Pen:       tool_.current = Tool::Pen;    break;
            case ToolbarAction::Eraser:    tool_.current = Tool::Eraser; break;
            case ToolbarAction::Lasso:     tool_.current = Tool::Lasso;  break;
            case ToolbarAction::Keyboard:
                tool_.keyboard_visible = !tool_.keyboard_visible;
                keyboard_.set_visible(tool_.keyboard_visible);
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
                break;
            case ToolbarAction::PrevPage:
                current_page_ = pages_clamp(note_, current_page_ - 1);
                break;
            case ToolbarAction::NextPage:
                current_page_ = pages_clamp(note_, current_page_ + 1);
                break;
            case ToolbarAction::Back:
                if (history_.can_back()) {
                    auto e = history_.pop_back_to_prev();
                    enter_note(e.note_id);
                    current_page_ = e.page;
                } else {
                    enter_browser();
                }
                break;
            case ToolbarAction::Browser: enter_browser(); break;
            case ToolbarAction::Undo:
                if (screen_ == Screen::Markdown) markdown_undo();
                break;
            default: break;
        }
        redraw();
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
