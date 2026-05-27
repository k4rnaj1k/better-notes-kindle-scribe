#pragma once
#include "strokes.h"

#include <string>
#include <vector>

namespace bn {

struct NavEntry {
    std::string note_id;
    int         page = 0;
};

class NavHistory {
public:
    void push(const NavEntry &e) { stack_.push_back(e); }
    bool can_back() const { return stack_.size() > 1; }
    NavEntry pop_back_to_prev() {
        if (stack_.size() >= 2) {
            stack_.pop_back();
            return stack_.back();
        }
        return stack_.empty() ? NavEntry{} : stack_.back();
    }
    const NavEntry *current() const {
        return stack_.empty() ? nullptr : &stack_.back();
    }
    void clear() { stack_.clear(); }
private:
    std::vector<NavEntry> stack_;
};

// Find the first link on the given page whose rect contains (x, y).
const Link *link_at(const Note &n, int page, double x, double y);

} // namespace bn
