// Markdown editor: buffer edits + undo, cursor/scroll, Obsidian-style inline
// title ↔ filename sync.

#include "app.h"

#include <algorithm>

namespace bn {

namespace {

// markdown_offset_at / markdown_caret need a cairo context to lay out text
// for hit-testing; font metrics are target-independent, so a throwaway 1x1
// surface suffices.
struct ScratchCairo {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t         *cr   = cairo_create(surf);
    ~ScratchCairo() { cairo_destroy(cr); cairo_surface_destroy(surf); }
};

std::string md_basename_no_ext(const std::string &path) {
    auto slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0)
        name.resize(name.size() - 3);
    return name;
}

// Display title shown as the H1 heading: the filename with '_' → ' '.
std::string md_title_from_filename(const std::string &basename_no_ext) {
    std::string out = basename_no_ext;
    for (auto &c : out) if (c == '_') c = ' ';
    return out;
}

// Filename (no extension) derived from the H1 title: trim, ' ' → '_', drop the
// characters that aren't safe in a filename. Case is preserved (Obsidian-like).
std::string md_filename_from_title(const std::string &title) {
    size_t a = title.find_first_not_of(" \t\r\n");
    size_t b = title.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::string t = title.substr(a, b - a + 1);
    std::string out;
    out.reserve(t.size());
    for (char c : t) {
        if (c == ' ' || c == '\t') out += '_';
        else if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                 c == '"' || c == '<' || c == '>' || c == '|' || c == '\0')
            continue;   // filesystem-unsafe — drop
        else out += c;
    }
    // No leading dot (would hide the file) or trailing separators.
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    while (!out.empty() && (out.back() == '_' || out.back() == '.'))
        out.pop_back();
    return out;
}

// If the buffer's first line is an H1 ("# ..."), return its trimmed text;
// otherwise "". This is the inline title the filename tracks.
std::string md_first_heading(const std::string &buf) {
    size_t nl = buf.find('\n');
    std::string line = buf.substr(0, nl == std::string::npos ? buf.size() : nl);
    if (line.size() >= 2 && line[0] == '#' && line[1] == ' ') {
        std::string t = line.substr(2);
        size_t a = t.find_first_not_of(" \t\r");
        size_t b = t.find_last_not_of(" \t\r");
        if (a == std::string::npos) return "";
        return t.substr(a, b - a + 1);
    }
    return "";
}

} // namespace

// --------------------------------------------------------- file / title

void App::flush_markdown() {
    if (markdown_path_.empty() || !markdown_dirty_) return;
    write_file(markdown_path_, md_.text);
    markdown_dirty_ = false;

    // Obsidian-style: rename the file to track its H1 inline title.
    std::string title = md_first_heading(md_.text);
    std::string want  = title.empty() ? "" : md_filename_from_title(title);
    std::string cur   = md_basename_no_ext(markdown_path_);
    if (!want.empty() && want != cur) {
        auto slash = markdown_path_.find_last_of('/');
        std::string dir = (slash == std::string::npos)
                              ? "" : markdown_path_.substr(0, slash);
        std::string newpath = (dir.empty() ? "" : dir + "/") + want + ".md";
        // Don't clobber an existing file; just keep the old name in that case.
        if (!path_exists(newpath) && rename_path(markdown_path_, newpath)) {
            markdown_path_ = newpath;
            status_ = "renamed → " + want + ".md";
        }
    }
    md_disk_mtime_ = file_mtime_ms(markdown_path_);   // refresh watch baseline
}

void App::ensure_markdown_title() {
    // Already starts with an "# ..." line → that's the inline title; leave it.
    if (!md_first_heading(md_.text).empty()) return;
    std::string title = md_title_from_filename(md_basename_no_ext(markdown_path_));
    std::string heading = "# " + title + "\n";
    if (!md_.text.empty() && md_.text[0] != '\n')
        heading += "\n";   // blank line between title and existing content
    md_.text.insert(0, heading);
    markdown_dirty_ = true;   // persist the added title on the next flush
}

// ---------------------------------------------------------------- edits

void App::markdown_snapshot() {
    // Cap history to keep memory bounded; ~200 snapshots is plenty even for
    // long sessions.
    if (markdown_history_.size() > 200)
        markdown_history_.erase(markdown_history_.begin());
    markdown_history_.emplace_back(md_.text, md_.cursor);
}

void App::markdown_undo() {
    if (markdown_history_.empty()) return;
    auto [buf, cur] = markdown_history_.back();
    markdown_history_.pop_back();
    md_.text   = std::move(buf);
    md_.cursor = std::min(cur, md_.text.size());
    markdown_dirty_ = true;
    md_last_edit_   = MdEdit::None;
    redraw();
}

void App::markdown_insert(const std::string &text) {
    md_.insert(text);
    markdown_dirty_ = true;
    md_last_edit_   = MdEdit::None;
}

void App::markdown_edit(const KeyEvent &e) {
    // Editing needs the caret, which only the source view has.
    if (tool_.markdown_pretty) {
        tool_.markdown_pretty = false;
        markdown_scroll_ = 0.0;
        md_lines_.clear();
        redraw();
    }
    // A run of typing (or of deleting) is one undo step; each word starts a
    // new one, as does switching between typing and deleting.
    MdEdit kind = MdEdit::None;
    if (e.key == Key::Text)                                    kind = MdEdit::Insert;
    else if (e.key == Key::Backspace || e.key == Key::Delete)  kind = MdEdit::Delete;
    if (kind != MdEdit::None &&
        (kind != md_last_edit_ || (kind == MdEdit::Insert && e.text == " ")))
        markdown_snapshot();
    md_last_edit_ = kind;

    bool changed = false;
    if (md_.apply(e, &changed) && changed) markdown_dirty_ = true;
}

void App::markdown_newline() {
    // Snapshot before each new line so undo lands on a sensible boundary.
    markdown_snapshot();
    md_.clamp();

    // Continue the list / quote the cursor is in: "- ", "* ", "+ ", "- [ ] ",
    // "> " and "N. " carry over to the new line with the same indentation.
    size_t ls = md_.cursor == 0 ? std::string::npos
                                : md_.text.rfind('\n', md_.cursor - 1);
    ls = (ls == std::string::npos) ? 0 : ls + 1;
    std::string line = md_.text.substr(ls, md_.cursor - ls);
    size_t ind = line.find_first_not_of(" \t");
    if (ind == std::string::npos) ind = line.size();
    std::string indent = line.substr(0, ind);
    std::string rest   = line.substr(ind);

    std::string marker, next;   // marker as typed, and its continuation
    auto starts = [&](const char *p) { return rest.rfind(p, 0) == 0; };
    if (starts("- [ ] ") || starts("- [x] ") || starts("- [X] ")) {
        marker = rest.substr(0, 6); next = "- [ ] ";
    } else if (starts("- ") || starts("* ") || starts("+ ") || starts("> ")) {
        marker = next = rest.substr(0, 2);
    } else {
        size_t d = 0;
        while (d < rest.size() && rest[d] >= '0' && rest[d] <= '9') ++d;
        if (d > 0 && d < 9 && rest.compare(d, 2, ". ") == 0) {
            marker = rest.substr(0, d + 2);
            next   = std::to_string(std::stoi(rest.substr(0, d)) + 1) + ". ";
        }
    }

    bool at_line_end = md_.cursor >= md_.text.size() || md_.text[md_.cursor] == '\n';
    if (!marker.empty() && rest == marker && at_line_end) {
        // Enter on an empty item ends the list: drop the marker instead.
        md_.text.erase(ls + ind, marker.size());
        md_.cursor = ls + ind;
        markdown_dirty_ = true;
        md_last_edit_   = MdEdit::None;
        return;
    }
    markdown_insert("\n" + indent + next);
}

void App::markdown_wrap(const std::string &marker) {
    // No selection model, so insert an empty pair and park the caret inside.
    markdown_edit(key_text(marker + marker));
    md_.cursor -= marker.size();
}

// ---------------------------------------------------- cursor / scroll

double App::markdown_band_top() const { return toolbar_.height() + 12; }

double App::markdown_band_bottom() const {
    return keyboard_.visible() ? keyboard_.top_y() : (double)win_h_;
}

void App::redraw_markdown_body() {
    // Repaint only the text band (below the toolbar, above the keyboard). A
    // full redraw() here fired a slow whole-screen e-ink refresh on every
    // keystroke, which is what made typing feel laggy.
    double top = toolbar_.height();
    redraw_rect(0, top, (double)win_w_, markdown_band_bottom() - top);
}

void App::clamp_markdown_scroll() {
    double view = markdown_band_bottom() - markdown_band_top();
    double maxs = std::max(0.0, markdown_content_h_ - view);
    markdown_scroll_ = std::clamp(markdown_scroll_, 0.0, maxs);
}

void App::markdown_set_cursor_from_tap(double x, double y) {
    if (md_lines_.empty()) return;
    ScratchCairo sc;
    md_.cursor = markdown_offset_at(sc.cr, md_.text, md_lines_, x, y);
    md_last_edit_ = MdEdit::None;
}

void App::markdown_move_cursor_vertical(int dir) {
    if (md_lines_.empty()) return;
    ScratchCairo sc;
    double cx, cy0, cy1;
    if (!markdown_caret(sc.cr, md_.text, md_lines_, md_.cursor, &cx, &cy0, &cy1))
        return;
    // Index of the line the cursor is currently on.
    int cur = 0;
    for (int i = 0; i < (int)md_lines_.size(); ++i) {
        const MdLineBox &b = md_lines_[i];
        if (md_.cursor >= b.off && md_.cursor <= b.off + b.len) { cur = i; break; }
        if (md_.cursor > b.off + b.len) cur = i;
    }
    int tgt = cur + dir;
    if (tgt >= 0 && tgt < (int)md_lines_.size()) {
        const MdLineBox &b = md_lines_[tgt];
        double midy = (b.y0 + b.y1) / 2.0;   // keep column x, snap to target row
        md_.cursor = markdown_offset_at(sc.cr, md_.text, md_lines_, cx, midy);
    }
    md_last_edit_ = MdEdit::None;
}

void App::markdown_page(int dir) {
    double delta = (markdown_band_bottom() - markdown_band_top()) * 0.9;
    // Move the caret the same distance it scrolls, keeping its column. The
    // line map is in the current frame's coordinates, so target the caret's
    // own y shifted by the scroll.
    if (!md_lines_.empty()) {
        ScratchCairo sc;
        double cx, cy0, cy1;
        if (markdown_caret(sc.cr, md_.text, md_lines_, md_.cursor, &cx, &cy0, &cy1))
            md_.cursor = markdown_offset_at(sc.cr, md_.text, md_lines_, cx,
                                            (cy0 + cy1) / 2.0 + dir * delta);
    }
    markdown_scroll_ += dir * delta;
    clamp_markdown_scroll();
    md_last_edit_ = MdEdit::None;
    redraw_markdown_body();
}

} // namespace bn
