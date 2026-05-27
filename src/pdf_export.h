#pragma once
#include "strokes.h"

#include <string>

namespace bn {

bool export_pdf(const std::string &out_path, const Note &n,
                double page_w_pt, double page_h_pt);

} // namespace bn
