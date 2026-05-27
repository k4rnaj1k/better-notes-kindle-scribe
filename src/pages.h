#pragma once
#include "strokes.h"

namespace bn {

void pages_append(Note &n, TemplateId tmpl);
bool pages_remove(Note &n, int idx);
int  pages_clamp(const Note &n, int idx);

} // namespace bn
