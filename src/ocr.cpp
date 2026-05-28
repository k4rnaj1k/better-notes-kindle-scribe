#include "ocr.h"
#include "canvas.h"

#include <cairo/cairo.h>
#include <chrono>
#include <regex>

#if BN_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
#include <leptonica/allheaders.h>
#endif

#include <unordered_map>

namespace bn {

namespace {

std::vector<std::string> extract_wiki_links(const std::string &text) {
    std::vector<std::string> out;
    std::regex re("\\[\\[([^\\]]+)\\]\\]");
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) out.push_back((*it)[1].str());
    return out;
}

} // namespace

void Ocr::set_enabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on && !running_) {
        running_ = true;
        thread_ = std::thread([this]{ thread_main(); });
    } else if (!on) {
        // Worker stays alive but idle until re-enabled or stop()'d.
    }
}

void Ocr::stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        running_ = false;
        dirty_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Ocr::notify(const Note &n, int page, int w_px, int h_px) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> g(mu_);
    pending_note_ = n;
    pending_page_ = page;
    pending_w_    = w_px;
    pending_h_    = h_px;
    dirty_        = true;
    cv_.notify_one();
}

void Ocr::thread_main() {
    while (running_) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]{ return dirty_ || !running_; });
        if (!running_) break;
        // Debounce: 1 s idle before running.
        auto last_change = std::chrono::steady_clock::now();
        dirty_ = false;
        Note snapshot = pending_note_;
        int  page = pending_page_;
        int  w = pending_w_, h = pending_h_;
        lk.unlock();

        bool re_armed = false;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::unique_lock<std::mutex> lk2(mu_);
            if (dirty_) {
                last_change = std::chrono::steady_clock::now();
                dirty_ = false;
                snapshot = pending_note_;
                page = pending_page_; w = pending_w_; h = pending_h_;
                re_armed = true;
                continue;
            }
            if (std::chrono::steady_clock::now() - last_change >
                std::chrono::seconds(1)) {
                lk2.unlock();
                run_once(snapshot, page, w, h);
                break;
            }
            (void)re_armed;
        }
    }
}

void Ocr::run_once(const Note &n, int page, int w_px, int h_px) {
#if BN_HAVE_TESSERACT
    if (page < 0 || page >= (int)n.pages.size() || !enabled_) return;
    if (w_px <= 0 || h_px <= 0) return;

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w_px, h_px);
    cairo_t *cr = cairo_create(surf);
    canvas_render_page(cr, n.pages[page], (double)w_px, (double)h_px);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    static tesseract::TessBaseAPI *tess = nullptr;
    if (!tess) {
        tess = new tesseract::TessBaseAPI();
        if (tess->Init(tessdata_.empty() ? nullptr : tessdata_.c_str(),
                       "eng") != 0) {
            log_err("ocr: tesseract init failed (tessdata=%s)",
                    tessdata_.c_str());
            delete tess; tess = nullptr;
            cairo_surface_destroy(surf);
            return;
        }
    }

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    tess->SetImage(data, w_px, h_px, 4, stride);
    char *text = tess->GetUTF8Text();
    OcrResult r;
    r.page = page;
    if (text) r.text = text;
    r.wiki_links = extract_wiki_links(r.text);
    delete[] text;

    // Build a map word_text → bbox so we can attach a real rect to each
    // [[wiki-link]] target. Tesseract's iterator yields words in reading
    // order; we keep the first occurrence (good enough for now).
    std::unordered_map<std::string, Rect> word_rects;
    tesseract::ResultIterator *it = tess->GetIterator();
    if (it) {
        do {
            const char *w = it->GetUTF8Text(tesseract::RIL_WORD);
            if (w && *w) {
                int x0, y0, x1, y1;
                if (it->BoundingBox(tesseract::RIL_WORD, &x0, &y0, &x1, &y1)) {
                    Rect rect;
                    rect.x = (double)x0;
                    rect.y = (double)y0;
                    rect.w = (double)(x1 - x0);
                    rect.h = (double)(y1 - y0);
                    word_rects.emplace(w, rect);
                }
            }
            delete[] w;
        } while (it->Next(tesseract::RIL_WORD));
        delete it;
    }
    // For each [[name]] candidate, look up the first matching word; the
    // link rect can later be expanded to span all the words in the name.
    r.word_rects.reserve(r.wiki_links.size());
    for (auto &name : r.wiki_links) {
        Rect rect{};
        // Try the first word of the wiki-link name.
        auto first_word_end = name.find(' ');
        std::string first = (first_word_end == std::string::npos)
                            ? name : name.substr(0, first_word_end);
        auto it_rect = word_rects.find(first);
        if (it_rect != word_rects.end()) rect = it_rect->second;
        r.word_rects.push_back(rect);
    }

    cairo_surface_destroy(surf);
    if (cb_) cb_(r);
#else
    (void)n; (void)page; (void)w_px; (void)h_px;
#endif
}

std::string Ocr::recognize(const std::vector<Stroke> &strokes,
                           int w_px, int h_px) {
#if BN_HAVE_TESSERACT
    if (strokes.empty() || w_px <= 0 || h_px <= 0) return "";

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w_px, h_px);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);   // white page for good contrast
    cairo_paint(cr);
    for (auto &s : strokes) canvas_render_stroke(cr, s);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    // A dedicated instance kept alive across calls (init is slow). Separate
    // from the background worker's so the two never race.
    static tesseract::TessBaseAPI *tess = nullptr;
    if (!tess) {
        tess = new tesseract::TessBaseAPI();
        if (tess->Init(tessdata_.empty() ? nullptr : tessdata_.c_str(),
                       "eng") != 0) {
            log_err("ocr: recognize() tesseract init failed (tessdata=%s)",
                    tessdata_.c_str());
            delete tess; tess = nullptr;
            cairo_surface_destroy(surf);
            return "";
        }
    }
    // Treat the box as a single text line (handles one word or a short phrase).
    tess->SetPageSegMode(tesseract::PSM_SINGLE_LINE);

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    tess->SetImage(data, w_px, h_px, 4, stride);
    char *text = tess->GetUTF8Text();
    std::string out = text ? text : "";
    delete[] text;
    cairo_surface_destroy(surf);

    // Trim surrounding whitespace/newlines Tesseract appends.
    size_t a = out.find_first_not_of(" \t\r\n");
    size_t b = out.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return out.substr(a, b - a + 1);
#else
    (void)strokes; (void)w_px; (void)h_px;
    return "";
#endif
}

} // namespace bn
