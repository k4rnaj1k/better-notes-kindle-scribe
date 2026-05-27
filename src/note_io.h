#pragma once
#include "strokes.h"

#include <string>

namespace bn {

// note.json layout (see plan):
//   { id, title, template, pages: ["page-001", ...],
//     links: [{page, rect, target}], ocr_text: {"1": "..."} }
//
// Each referenced page file is "<id>/<name>.json" containing strokes.

bool load_note(const std::string &dir, Note &out);
bool save_note(const std::string &dir, const Note &n);

// PNG snapshot of a fully-rendered page (template + strokes). Used by the
// note browser to show thumbnails and as a cache for fast first-paint.
bool save_page_snapshot(const std::string &dir, int page_index,
                        int width_px, int height_px, const Note &n);

} // namespace bn
