#include "markdown.h"

#include <pango/pangocairo.h>
#include <sstream>
#include <vector>

namespace bn {

namespace {

// Escape and inline-format a single block of text. Recognised inlines:
//   **bold**, *italic*, `code`. Maps to Pango markup.
std::string inline_pango(const std::string &raw) {
    std::string out; out.reserve(raw.size() + 16);
    bool bold = false, ital = false, code = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            out += raw[++i]; continue;
        }
        if (!code && c == '*' && i + 1 < raw.size() && raw[i+1] == '*') {
            out += bold ? "</b>" : "<b>"; bold = !bold; ++i; continue;
        }
        if (!code && c == '*') {
            out += ital ? "</i>" : "<i>"; ital = !ital; continue;
        }
        if (c == '`') {
            out += code ? "</tt>" : "<tt>"; code = !code; continue;
        }
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

double render_markdown(cairo_t *cr, double x, double y0,
                       double width, const std::string &src) {
    double y = y0;
    std::istringstream iss(src);
    std::string line;

    bool in_code = false;
    std::string code_buf;
    std::string para_buf;

    auto flush_para = [&]() {
        if (para_buf.empty()) return;
        y = layout_markup(cr, x, y, width, inline_pango(para_buf), "Sans 14");
        para_buf.clear();
    };

    auto flush_code = [&]() {
        if (code_buf.empty()) { in_code = false; return; }
        cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
        cairo_rectangle(cr, x - 4, y - 2, width + 8, 0); // bg drawn underneath
        cairo_fill(cr);
        std::string esc;
        for (char c : code_buf) {
            if (c == '&') esc += "&amp;";
            else if (c == '<') esc += "&lt;";
            else if (c == '>') esc += "&gt;";
            else esc += c;
        }
        y = layout_markup(cr, x, y, width, "<tt>" + esc + "</tt>", "Monospace 12");
        code_buf.clear();
        in_code = false;
    };

    while (std::getline(iss, line)) {
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (in_code) flush_code();
            else { flush_para(); in_code = true; }
            continue;
        }
        if (in_code) { code_buf += line + "\n"; continue; }

        if (line.empty()) { flush_para(); y += 6; continue; }

        if (line.size() > 2 && line.substr(0, 4) == "### ") {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(4)), "Sans Bold 16");
            continue;
        }
        if (line.size() > 2 && line.substr(0, 3) == "## ") {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(3)), "Sans Bold 20");
            continue;
        }
        if (line.size() > 1 && line.substr(0, 2) == "# ") {
            flush_para();
            y = layout_markup(cr, x, y, width,
                              inline_pango(line.substr(2)), "Sans Bold 28");
            continue;
        }
        if (line.size() > 1 && (line.substr(0, 2) == "- " ||
                                 line.substr(0, 2) == "* ")) {
            flush_para();
            y = layout_markup(cr, x + 18, y, width - 18,
                              "\xe2\x80\xa2  " + inline_pango(line.substr(2)),
                              "Sans 14");
            continue;
        }
        if (line == "---" || line == "***") {
            flush_para();
            cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, x, y + 4);
            cairo_line_to(cr, x + width, y + 4);
            cairo_stroke(cr);
            y += 12;
            continue;
        }
        if (line.size() > 1 && line.substr(0, 2) == "> ") {
            flush_para();
            cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
            cairo_rectangle(cr, x, y, 3, 24); cairo_fill(cr);
            y = layout_markup(cr, x + 12, y, width - 12,
                              "<i>" + inline_pango(line.substr(2)) + "</i>",
                              "Sans 14");
            continue;
        }

        if (!para_buf.empty()) para_buf += ' ';
        para_buf += line;
    }
    flush_para();
    if (in_code) flush_code();
    return y;
}

} // namespace bn
