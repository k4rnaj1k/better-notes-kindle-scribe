#include "markdown.h"

#include <pango/pangocairo.h>
#include <map>
#include <sstream>

namespace bn {

namespace {

// Base fonts. Bigger than the old renderer so prose is comfortable to read on
// the Scribe's 300dpi panel.
const char *kFontBody    = "Sans 20";
const char *kFontH1      = "Sans Bold 32";
const char *kFontH2      = "Sans Bold 27";
const char *kFontH3      = "Sans Bold 23";
const char *kFontCode    = "Monospace 17";

bool starts_with(const std::string &s, const char *p) {
    size_t n = 0; while (p[n]) ++n;
    return s.size() >= n && s.compare(0, n, p) == 0;
}

// True for an ordered-list item: one or more digits, then ". " or ") ".
// (Without this, "1. a"/"2. b" lines fall into the paragraph branch and get
// joined with spaces, dropping the line breaks.)
bool is_ordered_item(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    return i > 0 && i + 1 < line.size() &&
           (line[i] == '.' || line[i] == ')') && line[i + 1] == ' ';
}

// Pick a font for a source line. We render the RAW line bytes (no markup
// stripping) so the on-screen text matches the buffer 1:1 — that keeps tap →
// offset and offset → caret exact. Structure only changes the font.
const char *line_font(const std::string &line, bool in_code) {
    if (in_code)                  return kFontCode;
    if (starts_with(line, "### ")) return kFontH3;
    if (starts_with(line, "## "))  return kFontH2;
    if (starts_with(line, "# "))   return kFontH1;
    return kFontBody;
}

PangoLayout *make_line_layout(cairo_t *cr, const std::string &text,
                              const char *font, double width) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, (int)(width * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(l, text.c_str(), (int)text.size());
    return l;
}

// Split src into (offset, length) line spans on '\n'. A trailing newline (or
// an empty buffer) yields a final empty line so the caret can sit there.
void split_lines(const std::string &src,
                 std::vector<std::pair<size_t, size_t>> &out) {
    size_t i = 0;
    for (;;) {
        size_t nl = src.find('\n', i);
        size_t end = (nl == std::string::npos) ? src.size() : nl;
        out.push_back({i, end - i});
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
}

bool is_fence(const std::string &line) { return starts_with(line, "```"); }

} // namespace

double render_markdown(cairo_t *cr, double x, double y0,
                       double width, const std::string &src,
                       std::vector<MdLineBox> *out_lines) {
    if (out_lines) out_lines->clear();

    std::vector<std::pair<size_t, size_t>> spans;
    split_lines(src, spans);

    double y = y0;
    bool in_code = false;
    for (auto &sp : spans) {
        std::string text = src.substr(sp.first, sp.second);
        bool fence = is_fence(text);
        const char *font = line_font(text, in_code || fence);

        PangoLayout *l = make_line_layout(cr, text, font, width);
        int tw, th; pango_layout_get_size(l, &tw, &th);
        double h = th / (double)PANGO_SCALE;

        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);

        if (out_lines)
            out_lines->push_back(MdLineBox{sp.first, sp.second, x, y,
                                           y + h, width, font});

        y += h + 4.0;
        if (fence) in_code = !in_code;
    }
    return y;
}

size_t markdown_offset_at(cairo_t *cr, const std::string &src,
                          const std::vector<MdLineBox> &lines,
                          double px, double py) {
    if (lines.empty()) return 0;
    if (py < lines.front().y0) return lines.front().off;
    const MdLineBox &last = lines.back();
    if (py >= last.y1) return last.off + last.len;

    for (const MdLineBox &b : lines) {
        if (py < b.y0 || py >= b.y1) continue;
        std::string text = src.substr(b.off, b.len);
        PangoLayout *l = make_line_layout(cr, text, b.font.c_str(), b.width);
        int idx = 0, trailing = 0;
        pango_layout_xy_to_index(l,
            (int)((px - b.x) * PANGO_SCALE),
            (int)((py - b.y0) * PANGO_SCALE), &idx, &trailing);
        g_object_unref(l);
        size_t off = b.off + (size_t)idx + (size_t)trailing;
        if (off < b.off) off = b.off;
        if (off > b.off + b.len) off = b.off + b.len;
        return off;
    }
    return last.off + last.len;
}

bool markdown_caret(cairo_t *cr, const std::string &src,
                    const std::vector<MdLineBox> &lines, size_t cursor,
                    double *cx, double *cy0, double *cy1) {
    if (lines.empty()) return false;

    const MdLineBox *box = &lines.front();
    for (const MdLineBox &b : lines) {
        if (cursor >= b.off && cursor <= b.off + b.len) { box = &b; break; }
        if (cursor > b.off + b.len) box = &b;  // keep latest line we passed
    }

    size_t idx = cursor >= box->off ? cursor - box->off : 0;
    if (idx > box->len) idx = box->len;

    std::string text = src.substr(box->off, box->len);
    PangoLayout *l = make_line_layout(cr, text, box->font.c_str(), box->width);
    PangoRectangle strong;
    pango_layout_get_cursor_pos(l, (int)idx, &strong, nullptr);
    g_object_unref(l);

    *cx  = box->x  + strong.x / (double)PANGO_SCALE;
    *cy0 = box->y0 + strong.y / (double)PANGO_SCALE;
    *cy1 = *cy0 + strong.height / (double)PANGO_SCALE;
    return true;
}

namespace {

// Convert inline markdown (**bold**, *italic*, `code`) to Pango markup,
// escaping the rest.
std::string inline_pango(const std::string &raw) {
    std::string out; out.reserve(raw.size() + 16);
    bool bold = false, ital = false, code = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) { out += raw[++i]; continue; }
        if (!code && c == '*' && i + 1 < raw.size() && raw[i+1] == '*') {
            out += bold ? "</b>" : "<b>"; bold = !bold; ++i; continue;
        }
        if (!code && c == '*') {
            out += ital ? "</i>" : "<i>"; ital = !ital; continue;
        }
        if (c == '`') { out += code ? "</tt>" : "<tt>"; code = !code; continue; }
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out += c;
        }
    }
    if (bold) out += "</b>";
    if (ital) out += "</i>";
    if (code) out += "</tt>";
    return out;
}

double layout_markup(cairo_t *cr, double x, double y, double width,
                     const std::string &markup, const char *fontspec) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(fontspec);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_markup(layout, markup.c_str(), -1);
    int tw, th; pango_layout_get_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return y + th / (double)PANGO_SCALE + 4;
}

// PNG surfaces loaded for ![](...) images, keyed by resolved absolute path.
// Held for the process lifetime (a note references a handful of images), so the
// per-frame pretty render never re-hits disk. nullptr is cached too, so a
// missing/non-PNG file isn't retried every frame.
std::map<std::string, cairo_surface_t *> g_img_cache;

cairo_surface_t *load_image(const std::string &path) {
    auto it = g_img_cache.find(path);
    if (it != g_img_cache.end()) return it->second;
    cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
    if (s && cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        s = nullptr;
    }
    g_img_cache.emplace(path, s);
    return s;
}

// Render a standalone ![alt](path) image line. Loads the PNG (resolved relative
// to base_dir) and scales it down to fit the column width; shows an italic
// placeholder when it can't be loaded. Returns the new y, or -1 if `line` is
// not a valid image link (so the caller treats it as text).
double render_image_line(cairo_t *cr, double x, double y, double width,
                         const std::string &line, const std::string &base_dir) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line.compare(a, 2, "![") != 0) return -1;
    size_t mid = line.find("](", a);
    if (mid == std::string::npos) return -1;
    size_t end = line.find(')', mid + 2);
    if (end == std::string::npos) return -1;
    std::string alt  = line.substr(a + 2, mid - (a + 2));
    std::string path = line.substr(mid + 2, end - (mid + 2));

    std::string resolved = path;
    if (!path.empty() && path[0] != '/' && !base_dir.empty())
        resolved = base_dir + "/" + path;

    cairo_surface_t *img = resolved.empty() ? nullptr : load_image(resolved);
    if (!img) {
        std::string ph = "[image: " + (alt.empty() ? path : alt) + "]";
        return layout_markup(cr, x, y, width,
                             "<i>" + inline_pango(ph) + "</i>", kFontBody);
    }
    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return y;
    double scale = (iw > width) ? width / (double)iw : 1.0;
    double dh = ih * scale;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return y + dh + 8.0;
}

} // namespace

double render_markdown_pretty(cairo_t *cr, double x, double y0,
                             double width, const std::string &src,
                             const std::string &base_dir) {
    double y = y0;
    std::istringstream iss(src);
    std::string line;

    bool in_code = false;
    std::string code_buf, para_buf;
    int ordered_counter = 0;   // running number for the current ordered list

    auto flush_para = [&]() {
        if (para_buf.empty()) return;
        y = layout_markup(cr, x, y, width, inline_pango(para_buf), kFontBody);
        para_buf.clear();
    };
    auto flush_code = [&]() {
        if (!code_buf.empty()) {
            std::string esc;
            for (char c : code_buf) {
                if (c == '&') esc += "&amp;";
                else if (c == '<') esc += "&lt;";
                else if (c == '>') esc += "&gt;";
                else esc += c;
            }
            y = layout_markup(cr, x, y, width, "<tt>" + esc + "</tt>", kFontCode);
            code_buf.clear();
        }
        in_code = false;
    };

    while (std::getline(iss, line)) {
        if (line.size() >= 3 && line.compare(0, 3, "```") == 0) {
            if (in_code) flush_code();
            else { flush_para(); in_code = true; }
            ordered_counter = 0;
            continue;
        }
        if (in_code) { code_buf += line + "\n"; continue; }
        if (line.empty()) { flush_para(); y += 6; continue; }

        // Standalone image: ![alt](path). Flush any pending paragraph above it
        // first so it lands at the right y.
        {
            size_t a = line.find_first_not_of(" \t");
            if (a != std::string::npos && line.compare(a, 2, "![") == 0 &&
                line.find("](", a) != std::string::npos) {
                flush_para();
                double iy = render_image_line(cr, x, y, width, line, base_dir);
                if (iy >= 0) { y = iy; ordered_counter = 0; continue; }
            }
        }

        // Ordered-list numbering: items renumber sequentially (1, 2, 3…)
        // regardless of the digits typed. A blank line is handled above and
        // never reaches here, so it keeps the list going; any other block type
        // falls through here and resets the count.
        bool ordered = is_ordered_item(line);
        if (ordered) ++ordered_counter; else ordered_counter = 0;

        if (starts_with(line, "### ")) {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(4)), kFontH3);
        } else if (starts_with(line, "## ")) {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(3)), kFontH2);
        } else if (starts_with(line, "# ")) {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(2)), kFontH1);
        } else if (starts_with(line, "- ") || starts_with(line, "* ")) {
            flush_para();
            y = layout_markup(cr, x + 18, y, width - 18,
                              "\xe2\x80\xa2  " + inline_pango(line.substr(2)),
                              kFontBody);
        } else if (ordered) {
            // Renumber sequentially, dropping whatever digits the user typed.
            flush_para();
            size_t i = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
            std::string content = line.substr(i + 2);   // past "N. " / "N) "
            std::string marker  = std::to_string(ordered_counter) + ".  ";
            y = layout_markup(cr, x + 18, y, width - 18,
                              marker + inline_pango(content), kFontBody);
        } else if (line == "---" || line == "***") {
            flush_para();
            cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, x, y + 4); cairo_line_to(cr, x + width, y + 4);
            cairo_stroke(cr);
            y += 12;
        } else if (starts_with(line, "> ")) {
            flush_para();
            cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
            cairo_rectangle(cr, x, y, 3, 24); cairo_fill(cr);
            y = layout_markup(cr, x + 12, y, width - 12,
                              "<i>" + inline_pango(line.substr(2)) + "</i>",
                              kFontBody);
        } else {
            if (!para_buf.empty()) para_buf += ' ';
            para_buf += line;
        }
    }
    flush_para();
    if (in_code) flush_code();
    return y;
}

} // namespace bn
