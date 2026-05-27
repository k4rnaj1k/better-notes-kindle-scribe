#include "app.h"

#include "canvas.h"
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
    return static_cast<App *>(self)->on_button_press(e->x, e->y) ? TRUE : FALSE;
}
gboolean cb_release(GtkWidget *, GdkEventButton *e, gpointer self) {
    return static_cast<App *>(self)->on_button_release(e->x, e->y) ? TRUE : FALSE;
}
gboolean cb_motion(GtkWidget *, GdkEventMotion *e, gpointer self) {
    return static_cast<App *>(self)->on_motion(e->x, e->y) ? TRUE : FALSE;
}
void cb_destroy(GtkWidget *, gpointer) { gtk_main_quit(); }

gboolean cb_process_now(gpointer self) {
    static_cast<App *>(self)->process_async_events();
    return G_SOURCE_REMOVE;  // one-shot; the pen thread schedules new ones
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
    // Inflate slightly so antialiased edges aren't clipped.
    const int pad = 2;
    int ix = (int)std::floor(x) - pad;
    int iy = (int)std::floor(y) - pad;
    int iw = (int)std::ceil(w) + 2 * pad;
    int ih = (int)std::ceil(h) + 2 * pad;
    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    if (iw <= 0 || ih <= 0) return;
    gtk_widget_queue_draw_area(canvas_, ix, iy, iw, ih);
}

int App::run(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

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
            markdown_buf_ += t;
            markdown_dirty_ = true;
            redraw();
        }
    });
    keyboard_.on_key([this](const std::string &k){
        if (screen_ == Screen::Markdown) {
            if (k == "Backspace" && !markdown_buf_.empty())
                markdown_buf_.pop_back();
            else if (k == "Enter")
                markdown_buf_ += '\n';
            markdown_dirty_ = true;
            redraw();
        }
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
                // Bbox of the new segment (previous endpoint → new point),
                // expanded by the stroke width so antialiased edges stay
                // inside the dirty rect.
                if (live_stroke_.pts.empty()) {
                    redraw_rect(p.x - live_stroke_.width,
                                p.y - live_stroke_.width,
                                live_stroke_.width * 2,
                                live_stroke_.width * 2);
                } else {
                    const Point &prev = live_stroke_.pts.back();
                    double minx = std::min(prev.x, p.x) - live_stroke_.width;
                    double miny = std::min(prev.y, p.y) - live_stroke_.width;
                    double maxx = std::max(prev.x, p.x) + live_stroke_.width;
                    double maxy = std::max(prev.y, p.y) + live_stroke_.width;
                    redraw_rect(minx, miny, maxx - minx, maxy - miny);
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
                                    win_h_ - (int)toolbar_.height() - 30);
                    }
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
                    Rect r = lasso_.bbox();
                    // For now: link target defaults to the first browser entry
                    // that isn't this note; this gets replaced by a picker UI
                    // in a follow-up. The selection is kept on screen so the
                    // user can still cancel by re-lassoing.
                    for (auto &e : index_.entries()) {
                        if (e.id != note_.id) {
                            Link l; l.page = current_page_; l.rect = r;
                            l.target = e.id;
                            note_.links.push_back(l);
                            note_.mark_dirty();
                            status_ = "linked → " + e.title;
                            break;
                        }
                    }
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
        if (!r.wiki_links.empty()) {
            // Heuristic: auto-create or link to the first match.
            for (auto &name : r.wiki_links) {
                std::string slug = slugify(name);
                bool found = false;
                for (auto &e : index_.entries())
                    if (e.id == slug) { found = true; break; }
                if (!found) index_.create_note(name, TemplateId::Blank);
                Link l; l.page = r.page; l.rect = {40, 40, 200, 30};
                l.target = slug;
                note_.links.push_back(l);
                note_.mark_dirty();
            }
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
        (double)std::max(0, win_h_ - (int)toolbar_.height() - 30);
}

void App::enter_browser() {
    if (note_.dirty) save_current();
    if (markdown_dirty_ && !markdown_path_.empty()) {
        write_file(markdown_path_, markdown_buf_);
        markdown_dirty_ = false;
    }
    screen_ = Screen::Browser;
    index_.open(notes_dir_);
    redraw();
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
    markdown_dirty_ = false;
    screen_ = Screen::Markdown;
    redraw();
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

void App::on_draw(cairo_t *cr, int win_w, int win_h) {
    win_w_ = win_w; win_h_ = win_h;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

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
        double page_h = win_h - toolbar_.height() - 30;
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
        double y = render_markdown(cr, 24, toolbar_.height() + 12,
                                   win_w - 48, markdown_buf_);
        (void)y;
    }

    if (keyboard_.visible()) {
        keyboard_.layout(win_w, win_h);
        keyboard_.draw(cr);
    }

    toolbar_.draw(cr, tool_, status_, current_page_,
                  (int)std::max<size_t>(1, note_.pages.size()));
}

bool App::on_button_press(double x, double y) {
    if (screen_ == Screen::Browser) {
        BrowserHit h = browser_.hit(x, y, index_.entries().size());
        if (h.action == BrowserAction::NewNote) {
            auto e = index_.create_note("Untitled", TemplateId::Blank);
            enter_note(e.id);
        } else if (h.action == BrowserAction::NewMarkdown) {
            auto e = index_.create_markdown("untitled");
            enter_markdown(e.path);
        } else if (h.action == BrowserAction::Open && h.entry_index >= 0) {
            const auto &e = index_.entries()[h.entry_index];
            if (e.is_markdown) enter_markdown(e.path);
            else enter_note(e.id);
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
        double py = y;
        const Link *l = link_at(note_, current_page_, x, py);
        if (l) {
            std::string target = l->target;
            // Treat .md targets as markdown
            std::string full = notes_dir_ + "/" + target;
            if (path_exists(full + ".md")) enter_markdown(full + ".md");
            else if (path_exists(full)) {
                struct stat st; stat(full.c_str(), &st);
                if (S_ISDIR(st.st_mode)) enter_note(target);
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
