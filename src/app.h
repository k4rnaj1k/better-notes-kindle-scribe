#pragma once
#include "filebrowser.h"
#include "index.h"
#include "keyboard.h"
#include "lasso.h"
#include "links.h"
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

namespace bn {

enum class Screen { Browser, NoteView, Markdown };

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

    // Called from pen_reader thread; just enqueues onto pen_queue_.
    void on_pen_sample(const PenSample &s);

    // GTK idle callback drains pen_queue_ + ocr_queue_.
    void process_async_events();

private:
    App();

    // --- screen routing ---
    void enter_browser();
    void enter_note(const std::string &id);
    void enter_markdown(const std::string &path);

    // --- note ops ---
    void save_current();
    void export_current_pdf();

    // --- pen sample → page coordinates ---
    void map_pen_to_page(const PenSample &s, double &px, double &py);

    void redraw();

    // --- state ---
    GtkWidget    *window_  = nullptr;
    GtkWidget    *canvas_  = nullptr;
    int           win_w_   = 0;
    int           win_h_   = 0;

    Screen        screen_  = Screen::Browser;
    ToolState     tool_;
    Toolbar       toolbar_;
    FileBrowser   browser_;
    Keyboard      keyboard_;
    Lasso         lasso_;
    NavHistory    history_;

    NotesIndex    index_;
    Note          note_;             // currently-open note
    int           current_page_ = 0;
    std::string   markdown_path_;
    std::string   markdown_buf_;
    bool          markdown_dirty_ = false;

    // Pen capture state
    Stroke        live_stroke_;
    bool          pen_down_       = false;
    PenButton     active_button_  = PenButton::None;

    // Thread plumbing
    PenReader     pen_;
    Ocr           ocr_;
    std::mutex          q_mu_;
    std::queue<PenSample> pen_queue_;
    std::queue<OcrResult> ocr_queue_;

    std::string   notes_dir_     = "/mnt/us/documents/betternotes";
    std::string   tessdata_dir_;
    std::string   status_;
    guint         idle_source_   = 0;
};

} // namespace bn
