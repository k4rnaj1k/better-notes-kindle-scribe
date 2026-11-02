// Stylus input: pen samples → strokes, eraser, lasso, the draw modal's
// canvas; plus background OCR results.

#include "app.h"

#include "canvas.h"
#include "inkfb.h"

#include <algorithm>

namespace bn {

namespace {

gboolean cb_process_now(gpointer self) {
    static_cast<App *>(self)->process_async_events();
    // G_SOURCE_REMOVE landed in GLib 2.32; the Kindle sysroot is older, so
    // use the literal it expands to.
    return FALSE;
}

InkRect ink_union(const InkRect &a, const InkRect &b) {
    if (a.w == 0) return b;
    int ax = std::min(a.x, b.x);
    int ay = std::min(a.y, b.y);
    int bx = std::max(a.x + a.w, b.x + b.w);
    int by = std::max(a.y + a.h, b.y + b.h);
    return {ax, ay, bx - ax, by - ay};
}

// Bbox of a committed stroke, padded by its width so the whole ink is covered.
Rect padded_bbox(const Stroke &s) {
    Rect bb = s.bbox();
    double w = s.width;
    return Rect{bb.x - w, bb.y - w, bb.w + 2 * w, bb.h + 2 * w};
}

} // namespace

void App::on_pen_sample(const PenSample &s) {
    bool was_empty;
    {
        std::lock_guard<std::mutex> g(q_mu_);
        was_empty = pen_queue_.empty();
        pen_queue_.push(s);
    }
    // Wake the GTK main loop immediately. g_idle_add is thread-safe. Only
    // schedule once per batch — the main-thread handler drains everything in
    // one pass, so re-scheduling for every queued sample just churns the loop.
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

    for (; !pq.empty(); pq.pop()) {
        const PenSample &s = pq.front();
        // The draw/OCR modal captures stylus input into its own canvas, on top
        // of (and instead of) the markdown screen below it.
        if (draw_open_) { last_pen_ms_ = now_ms(); handle_draw_sample(s); continue; }
        // A blocking modal owns input; don't let the stylus draw behind it.
        if (input_open_ || tmpl_open_ || picker_.open) continue;
        if (screen_ != Screen::NoteView) continue;
        handle_note_sample(s);
    }

    for (; !oq.empty(); oq.pop()) handle_ocr_result(oq.front());
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

Point App::pen_point(const PenSample &s, double px, double py) const {
    Point p;
    p.x = px; p.y = py;
    p.pressure = (float)s.pressure /
                 (float)std::max(1, pen_.calibration().max_pressure);
    p.t_ms = s.t_ms;
    return p;
}

void App::draw_live_segment(const Stroke &st, const Point &p, InkRect &bbox) {
    // When the fast framebuffer path is live, draw ONLY through it — a GTK
    // partial redraw would fire a second, slower e-ink refresh over the same
    // region and reintroduce lag. The clean cairo render happens on pen-up.
    // Without inkfb, fall back to GTK partial redraws so the stroke is still
    // visible mid-draw.
    if (st.pts.empty()) {
        if (!inkfb_available())
            redraw_rect(p.x - st.width, p.y - st.width, st.width * 2, st.width * 2);
        return;
    }
    const Point &prev = st.pts.back();
    if (inkfb_available()) {
        const PenStyle &ps = pen_style(st.pen_type);
        double lvl = ps.grey * ps.alpha + (1.0 - ps.alpha);
        InkRect r = inkfb_draw_segment(prev.x, prev.y, p.x, p.y,
                                       pen_live_width(st, prev, p), lvl, ps.spray);
        bbox = ink_union(bbox, r);
    } else {
        double minx = std::min(prev.x, p.x) - st.width;
        double miny = std::min(prev.y, p.y) - st.width;
        double maxx = std::max(prev.x, p.x) + st.width;
        double maxy = std::max(prev.y, p.y) + st.width;
        redraw_rect(minx, miny, maxx - minx, maxy - miny);
    }
}

void App::handle_note_sample(const PenSample &s) {
    // The dropdown is opened by finger (GTK) taps; a stylus touch on the
    // canvas dismisses it so drawing isn't blocked by a stale overlay.
    if (s.down && toolbar_.pen_menu_open()) {
        toolbar_.close_pen_menu();
        redraw_pen_menu_region();
    }
    // The pen's eraser end is a property of the physical stylus, not the
    // toolbar. Treat a Rubber sample as the eraser tool for THIS sample only
    // — no tool_.current mutation, no toolbar repaint, so the eraser bites the
    // instant the rubber touches.
    Tool eff = (s.tool == PenButton::Rubber) ? Tool::Eraser : tool_.current;

    // Note when the stylus was last seen so finger-only gestures (page
    // swipes) can ignore pointer events the pen also emits.
    last_pen_ms_ = now_ms();

    double px, py;
    map_pen_to_page(s, px, py);
    bool on_page = current_page_ < (int)note_.pages.size();

    if (eff == Tool::Pen) {
        if (s.down && !pen_down_) {
            pen_down_ = true;
            live_stroke_ = Stroke{};
            live_stroke_.tool     = Tool::Pen;
            live_stroke_.pen_type = tool_.pen_type;
            live_stroke_.width    =
                effective_pen_width(tool_.pen_type, tool_.pen_width_idx);
            // Calibration aid: dumps the raw device coords and where they
            // mapped to. Draw a dot in each corner and read these off the log
            // to derive the exact swap/invert combo.
            const PenCalibration &c = pen_.calibration();
            log_info("pen-down raw=(%d,%d) range x[%d..%d] y[%d..%d] "
                     "-> page=(%.0f,%.0f) win=%dx%d swap=%d ix=%d iy=%d",
                     s.x, s.y, c.min_x, c.max_x, c.min_y, c.max_y,
                     px, py, win_w_, win_h_,
                     (int)c.swap_xy, (int)c.invert_x, (int)c.invert_y);
        }
        if (pen_down_ && s.down) {
            Point p = pen_point(s, px, py);
            draw_live_segment(live_stroke_, p, live_ink_bbox_);
            live_stroke_.pts.push_back(p);
        }
        if (!s.down && pen_down_) {
            pen_down_ = false;
            if (!live_stroke_.pts.empty() && on_page) {
                Rect bb = padded_bbox(live_stroke_);
                note_.pages[current_page_].strokes.push_back(std::move(live_stroke_));
                live_stroke_ = Stroke{};
                note_.mark_dirty();
                // Fold the new stroke into the cached page bitmap so the next
                // blit (menu, flip back, partial repaint) shows it.
                update_page_cache_region(current_page_, bb);
                if (inkfb_available() && live_ink_bbox_.w > 0) {
                    // inkfb already painted the final stroke; a fast GC16
                    // settle over just the bbox cleans it up. Deliberately NO
                    // cairo re-render here — that fired a slow whole-bbox X
                    // refresh after every stroke and made styled pens "snap".
                    inkfb_settle(live_ink_bbox_);
                } else {
                    // No fast path: paint the committed stroke once through
                    // cairo, as a partial repaint of its bbox.
                    redraw_rect(bb);
                }
                live_ink_bbox_ = {0, 0, 0, 0};
            }
        }
    } else if (eff == Tool::Eraser) {
        if (s.down && on_page) {
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
                // Re-render just the erased footprint into the cached bitmap,
                // then repaint ONLY that footprint on screen.
                update_page_cache_region(current_page_, d);
                redraw_rect(d);
            }
        } else if (!s.down) {
            erase_active_ = false;
        }
    } else if (eff == Tool::Lasso) {
        if (s.down) {
            if (!pen_down_) { pen_down_ = true; lasso_.clear(); }
            Point p; p.x = px; p.y = py;
            lasso_.pts.push_back(p);
            // Repaint only the lasso's growing bbox, not the whole screen.
            Rect lb = lasso_.bbox();
            redraw_rect(lb.x - 4, lb.y - 4, lb.w + 8, lb.h + 8);
        } else if (pen_down_) {
            pen_down_ = false;
            lasso_.closed = lasso_.pts.size() >= 3;
            if (lasso_.closed) open_link_picker(lasso_.bbox());
            lasso_.clear();
            redraw();
        }
    }
}

void App::handle_draw_sample(const PenSample &s) {
    double px, py; map_pen_to_page(s, px, py);
    Rect canvas = draw_canvas_rect();
    bool ocr = draw_purpose_ == DrawPurpose::Ocr;
    // Recognise as you go so the result shows in the bar before Save — no
    // surprise about what gets inserted. Runs on the main thread, but the box
    // is small so the pause is brief.
    auto refresh_ocr = [&] {
        if (!ocr) return;
        ocr_preview_ = ocr_.recognize(draw_strokes_, (int)canvas.w, (int)canvas.h);
        redraw_rect(0, 0, (double)win_w_, canvas.y);   // repaint the top bar
    };

    if (s.tool == PenButton::Rubber) {
        if (s.down) {
            double ex0 = draw_erase_active_ ? draw_erase_px_ : px;
            double ey0 = draw_erase_active_ ? draw_erase_py_ : py;
            draw_erase_active_ = true; draw_erase_px_ = px; draw_erase_py_ = py;
            Page tmp; tmp.strokes = std::move(draw_strokes_);
            Rect d = canvas_erase_at(tmp, ex0, ey0, px, py, tool_.eraser_radius);
            draw_strokes_ = std::move(tmp.strokes);
            if (d.w > 0 || d.h > 0) redraw_rect(canvas);
        } else {
            draw_erase_active_ = false;
            refresh_ocr();
        }
        return;
    }

    if (s.down) {
        if (!draw_pen_down_) {
            if (!canvas.contains(px, py)) return;   // ignore touches on the bar
            draw_pen_down_ = true;
            draw_live_ = Stroke{};
            draw_live_.tool     = Tool::Pen;
            draw_live_.pen_type = tool_.pen_type;
            draw_live_.width    =
                effective_pen_width(tool_.pen_type, tool_.pen_width_idx);
            draw_ink_bbox_      = {0, 0, 0, 0};
        }
        Point p = pen_point(s, px, py);
        draw_live_segment(draw_live_, p, draw_ink_bbox_);
        draw_live_.pts.push_back(p);
    } else if (draw_pen_down_) {
        draw_pen_down_ = false;
        if (!draw_live_.pts.empty()) {
            draw_strokes_.push_back(std::move(draw_live_));
            draw_live_ = Stroke{};
        }
        // inkfb already painted the final stroke; just settle the bbox. The
        // cairo version only renders on Save/redraw.
        if (inkfb_available() && draw_ink_bbox_.w > 0)
            inkfb_settle(draw_ink_bbox_);
        else
            redraw_rect(canvas);
        draw_ink_bbox_ = {0, 0, 0, 0};
        refresh_ocr();
    }
}

void App::handle_ocr_result(const OcrResult &r) {
    if ((int)note_.ocr_text.size() <= r.page)
        note_.ocr_text.resize(r.page + 1);
    note_.ocr_text[r.page] = r.text;
    for (size_t i = 0; i < r.wiki_links.size(); ++i) {
        const auto &name = r.wiki_links[i];
        if (index_.resolve_link(name).empty()) {
            // Brand-new link target → create an .md in the current browser
            // folder (Obsidian default behaviour).
            index_.create_markdown(name);
        }
        Link l;
        l.page   = r.page;
        l.target = name;  // store the wiki-style name; resolved at follow time
        if (i < r.word_rects.size() && r.word_rects[i].w > 0) {
            // Tesseract gives page-image coords (0 = top of page), which is
            // exactly our page-local space — store as-is.
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

} // namespace bn
