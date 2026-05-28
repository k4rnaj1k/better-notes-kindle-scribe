#pragma once
#include "util.h"

#include <deque>
#include <string>
#include <vector>

namespace bn {

enum class Tool    { Pen, Eraser, Lasso, Hand };
enum class PenType { Pencil, Pen };

struct Stroke {
    Tool        tool     = Tool::Pen;
    PenType     pen_type = PenType::Pencil;
    double      width    = 1.4;
    std::vector<Point> pts;

    Rect bbox() const;
};

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
