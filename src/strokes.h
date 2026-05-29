#pragma once
#include "util.h"

#include <deque>
#include <string>
#include <vector>

namespace bn {

enum class Tool    { Pen, Eraser, Lasso, Hand };
enum class PenType { Pen, Pencil, Fountain, Marker, Highlighter, Spray };

struct Stroke {
    Tool        tool     = Tool::Pen;
    PenType     pen_type = PenType::Pen;
    // Effective base width in drawing units — already multiplied by the pen
    // type's scale at creation time, so bbox()/erase work without knowing the
    // type. Dynamic modulation (taper/direction) only ever stays <= this.
    double      width    = 3.0;
    std::vector<Point> pts;

    Rect bbox() const;
};

// Visual + dynamic behaviour for a pen type. Single source of truth shared by
// every render path: the cairo commit/reload/PDF renderer (canvas.cpp) and the
// inkfb live fast-path (which approximates grey via dithering).
struct PenStyle {
    double grey;        // ink lightness: 0 = black .. 1 = white
    double alpha;       // cairo opacity (< 1 for the translucent highlighter)
    double width_scale; // multiplies the selected base width
    bool   taper;       // width follows pen pressure (pencil)
    bool   directional; // width follows stroke direction, calligraphy nib (fountain)
    bool   grain;       // graphite speckle overlay (pencil)
    bool   spray;       // scattered dots instead of a solid line (spray can)
    bool   square_cap;  // square line caps (highlighter), else round
};

const PenStyle &pen_style(PenType t);
const char    *pen_type_name(PenType t);
PenType        pen_type_from_name(const std::string &n);

// Effective stroke width at point i, applying the pen type's taper/direction
// modulation to s.width. Used by both the cairo renderer and the live path.
double pen_width_at(const Stroke &s, size_t i);
// Same modulation for a live segment whose end point isn't in s.pts yet.
double pen_live_width(const Stroke &s, const Point &prev, const Point &cur);

enum class TemplateId { Blank, Ruled, Grid, Dot };

struct Link {
    int         page = 0;          // 0-based page index this link lives on
    Rect        rect;              // in page-local coordinates
    std::string target;            // note id or "<id>.md"
};

struct Page {
    TemplateId           tmpl = TemplateId::Blank;
    // Custom PNG template: a path (vault-relative, e.g. "templates/foo.png")
    // painted as the page background. When non-empty it overrides `tmpl`.
    std::string          bg_image;
    std::vector<Stroke>  strokes;
    // Per-page tags.
    std::vector<std::string> tags;
};

struct Note {
    std::string         id;
    std::string         title;
    TemplateId          default_template = TemplateId::Blank;
    // Custom PNG template inherited by new pages (vault-relative path).
    std::string         default_bg_image;
    std::vector<Page>   pages;
    std::vector<Link>   links;
    // Notebook-level tags.
    std::vector<std::string> tags;
    bool                dirty = false;

    // OCR results per page index → recognised text.
    std::vector<std::string> ocr_text;

    void mark_dirty() { dirty = true; }
};

const char *template_name(TemplateId t);
TemplateId  template_from_name(const std::string &n);

} // namespace bn
