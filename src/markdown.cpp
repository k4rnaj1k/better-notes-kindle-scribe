#include "markdown.h"

#include <pango/pangocairo.h>
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

} // namespace

double render_markdown_pretty(cairo_t *cr, double x, double y0,
                             double width, const std::string &src) {
    double y = y0;
    std::istringstream iss(src);
    std::string line;

    bool in_code = false;
    std::string code_buf, para_buf;

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
            continue;
        }
        if (in_code) { code_buf += line + "\n"; continue; }
        if (line.empty()) { flush_para(); y += 6; continue; }

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
