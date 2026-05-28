#include "pdf_export.h"
#include "canvas.h"

#include <cairo/cairo-pdf.h>

namespace bn {

bool export_pdf(const std::string &out_path, const Note &n,
                double w_pt, double h_pt,
                double src_w, double src_h,
                const std::function<void(int done, int total)> &on_progress) {
    cairo_surface_t *surf = cairo_pdf_surface_create(out_path.c_str(),
                                                     w_pt, h_pt);
    if (!surf) return false;
    cairo_t *cr = cairo_create(surf);

    // Scale from the on-screen capture space to PDF points. Guard against a
    // zero/unknown source size (fall back to 1:1).
    if (src_w <= 0) src_w = w_pt;
    if (src_h <= 0) src_h = h_pt;
    double sx = w_pt / src_w;
    double sy = h_pt / src_h;

    for (size_t i = 0; i < n.pages.size(); ++i) {
        if (on_progress) on_progress((int)i, (int)n.pages.size());
        cairo_pdf_surface_set_size(surf, w_pt, h_pt);

        cairo_save(cr);
        cairo_scale(cr, sx, sy);
        // Render the page (template + strokes) in capture-space dimensions;
        // the scale above maps it onto the PDF page.
        canvas_render_page(cr, n.pages[i], src_w, src_h);

        // Link annotations targeted at other notes appear as PDF link tags;
        // viewers that support cairo_tag_begin pick them up. The tag API
        // landed in cairo 1.16 — when building against an older sysroot
        // (e.g. the stock Kindle one) we just draw the highlight rectangle
        // without the link metadata.
        for (auto &l : n.links) {
            if (l.page != (int)i) continue;
#if BN_HAVE_CAIRO_TAG
            char attr[256];
            std::snprintf(attr, sizeof(attr),
                          "uri='betternotes:%s'", l.target.c_str());
            cairo_tag_begin(cr, "Link", attr);
#endif
            cairo_rectangle(cr, l.rect.x, l.rect.y, l.rect.w, l.rect.h);
            cairo_set_source_rgba(cr, 0, 0, 1, 0.06);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 1, 0.4);
            cairo_set_line_width(cr, 0.5);
            cairo_stroke(cr);
#if BN_HAVE_CAIRO_TAG
            cairo_tag_end(cr, "Link");
#endif
        }
        cairo_restore(cr);
        cairo_show_page(cr);
    }
    if (on_progress) on_progress((int)n.pages.size(), (int)n.pages.size());
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return true;
}

} // namespace bn
