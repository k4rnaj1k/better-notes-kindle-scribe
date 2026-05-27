#include "links.h"

namespace bn {

const Link *link_at(const Note &n, int page, double x, double y) {
    for (auto &l : n.links) {
        if (l.page != page) continue;
        if (l.rect.contains(x, y)) return &l;
    }
    return nullptr;
}

} // namespace bn
