#pragma once
#include "strokes.h"

#include <functional>
#include <string>

namespace bn {

// Strokes/links are stored in the on-screen pixel space they were drawn in
// (src_w x src_h). The PDF page is page_w_pt x page_h_pt. We scale the
// former onto the latter so the ink actually lands on the page.
//
// `title` (when non-empty and the cairo build supports it) is written as the
// PDF's document /Title metadata.
// `on_progress(done, total)` (optional) is invoked before each page is
// rendered and once more when finished, so callers can drive a progress bar.
bool export_pdf(const std::string &out_path, const Note &n,
                double page_w_pt, double page_h_pt,
                double src_w, double src_h,
                const std::string &title = "",
                const std::function<void(int done, int total)> &on_progress = {});

} // namespace bn
