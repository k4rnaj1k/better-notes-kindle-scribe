#pragma once
#include <cairo/cairo.h>
#include <string>

namespace bn {

// Render a markdown source string into the given cairo region. Supports the
// subset that's actually useful on an e-ink reader: headings (#, ##, ###),
// paragraphs, **bold**, *italic*, `code`, unordered lists (-, *), ordered
// lists (1.), blockquotes (>), fenced code blocks (```), horizontal rules.
//
// Returns the y-coordinate of the bottom of the rendered content so the
// caller can detect overflow.
double render_markdown(cairo_t *cr, double x, double y,
                       double width, const std::string &src);

} // namespace bn
