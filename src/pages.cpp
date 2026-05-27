#include "pages.h"

namespace bn {

void pages_append(Note &n, TemplateId tmpl) {
    n.pages.emplace_back();
    n.pages.back().tmpl = tmpl;
    n.mark_dirty();
}

bool pages_remove(Note &n, int idx) {
    if (idx < 0 || idx >= (int)n.pages.size()) return false;
    n.pages.erase(n.pages.begin() + idx);
    if (n.pages.empty()) n.pages.emplace_back();
    n.mark_dirty();
    return true;
}

int pages_clamp(const Note &n, int idx) {
    if (n.pages.empty()) return 0;
    if (idx < 0) return 0;
    if (idx >= (int)n.pages.size()) return (int)n.pages.size() - 1;
    return idx;
}

} // namespace bn
