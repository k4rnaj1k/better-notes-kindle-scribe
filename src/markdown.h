#pragma once
#include <cairo/cairo.h>
#include <cstddef>
#include <string>
#include <vector>

namespace bn {

// One source line as laid out on screen. The renderer draws each source line
// (split on '\n') with its own Pango layout and the exact source bytes, so a
// box maps 1:1 to a byte range in the buffer. That lets us turn a tap into a
// cursor offset and a cursor offset into a caret rectangle with no markup
// translation in between.
struct MdLineBox {
    size_t      off  = 0;     // byte offset of this line's first char in src
    size_t      len  = 0;     // byte length (excludes the trailing '\n')
    double      x    = 0;     // left edge where text is drawn
    double      y0   = 0;     // top y (drawing space, already scrolled)
    double      y1   = 0;     // bottom y
    double      width = 0;    // wrap width used for the layout
    std::string font;         // Pango font spec used (re-used for hit/caret)
};

// Render a markdown source string starting at (x, y0) with the given wrap
// width. This is a lightweight "source view": structural lines pick a font
// (headings larger, code monospace) but the exact source text is shown, so
// editing stays WYSIWYG with the cursor. Returns the bottom y of the content.
// When out_lines is non-null it is filled with the per-line layout map.
double render_markdown(cairo_t *cr, double x, double y0,
                       double width, const std::string &src,
                       std::vector<MdLineBox> *out_lines = nullptr);

// Pretty (read-oriented) renderer: formats **bold**, *italic*, `code`,
// headings, lists, blockquotes and rules with Pango markup. No cursor/tap
// mapping — used for the non-editing "preview" view. Returns the bottom y.
double render_markdown_pretty(cairo_t *cr, double x, double y0,
                             double width, const std::string &src);

// Map a drawing-space tap to a byte offset in src using the line map.
size_t markdown_offset_at(cairo_t *cr, const std::string &src,
                          const std::vector<MdLineBox> &lines,
                          double px, double py);

// Compute the caret rectangle for a cursor byte offset. Returns false if the
// cursor can't be placed (empty map). cx/cy0/cy1 are in drawing space.
bool markdown_caret(cairo_t *cr, const std::string &src,
                    const std::vector<MdLineBox> &lines, size_t cursor,
                    double *cx, double *cy0, double *cy1);

} // namespace bn
