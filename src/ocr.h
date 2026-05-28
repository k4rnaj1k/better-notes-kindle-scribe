#pragma once
// Background OCR worker. Lazily creates a tesseract instance the first time
// it is enabled. When enabled, the canvas notifies the OCR thread on each
// "stroke finished" event; the worker debounces 1 s of idleness, rasterises
// the page to an image surface, runs Tesseract, and posts the recognised
// text + bounding boxes back to the main thread via GLib's g_idle_add.
//
// Compiled out (becomes a no-op) when BN_HAVE_TESSERACT is not defined.

#include "strokes.h"
#include "util.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bn {

struct OcrResult {
    std::string text;
    int         page = 0;
    // Wiki-link candidates ([[name]]) extracted from text.
    std::vector<std::string> wiki_links;
    // Bounding rect for each wiki_links entry, page-coordinate space.
    // Same length as wiki_links when Tesseract's GetIterator() found the
    // word; empty rects (w=0) signal "fall back to placeholder".
    std::vector<Rect>        word_rects;
};

class Ocr {
public:
    using ResultCb = std::function<void(const OcrResult &)>;

    void set_tessdata_dir(const std::string &d) { tessdata_ = d; }
    void set_callback(ResultCb cb) { cb_ = std::move(cb); }

    bool enabled() const { return enabled_; }
    void set_enabled(bool on);

    // Tell the worker a stroke completed on `page` of the given note. Width
    // and height are the on-screen page dimensions in pixels.
    void notify(const Note &n, int page, int w_px, int h_px);

    // Synchronously OCR a set of strokes rendered onto a w_px×h_px white
    // canvas, returning the recognised text (trimmed). Runs on the caller's
    // thread (used by the markdown draw-box). Returns "" when Tesseract is
    // unavailable or nothing was recognised.
    std::string recognize(const std::vector<Stroke> &strokes,
                           int w_px, int h_px);

    void stop();

private:
    void thread_main();
    void run_once(const Note &n, int page, int w_px, int h_px);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::mutex        mu_;
    std::condition_variable cv_;

    // Latest pending request snapshot (copied so the canvas can be edited
    // concurrently).
    Note pending_note_;
    int  pending_page_ = -1;
    int  pending_w_    = 0;
    int  pending_h_    = 0;
    bool dirty_        = false;

    ResultCb    cb_;
    std::string tessdata_;
};

} // namespace bn
