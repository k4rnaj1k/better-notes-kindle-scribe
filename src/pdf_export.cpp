#include "pdf_export.h"
#include "canvas.h"
#include "strokes.h"

#include <cairo/cairo-pdf.h>

namespace bn {

namespace {

// Styled pens (spray scatter, pencil grain, fountain/pencil variable width)
// emit thousands of tiny vector elements per stroke. Emitting those straight
// into the PDF content stream bloats the file and is slow to serialise, so a
// page that crosses this budget is rasterised to a single embedded image
// instead. Plain pen/marker pages (one polyline per stroke) stay crisp vector.
bool page_is_heavy(const Page &p) {
    long est = 0;
    for (const auto &s : p.strokes) {
        const PenStyle &ps = pen_style(s.pen_type);
        long pts = (long)s.pts.size();
        if      (ps.spray)                  est += pts * 8;   // scattered dots
        else if (ps.grain)                  est += pts * 4;   // graphite specks
        else if (ps.taper || ps.directional) est += pts * 2;  // disc + quad/seg
        else                                est += 1;         // one polyline
        if (est > 25000) return true;
    }
    return false;
}

} // namespace

bool export_pdf(const std::string &out_path, const Note &n,
                double w_pt, double h_pt,
                double src_w, double src_h,
                const std::string &title,
                const std::function<void(int done, int total)> &on_progress) {
    cairo_surface_t *surf = cairo_pdf_surface_create(out_path.c_str(),
                                                     w_pt, h_pt);
    if (!surf) return false;
#if BN_HAVE_CAIRO_TAG
    // cairo_pdf_surface_set_metadata + CAIRO_PDF_METADATA_TITLE landed in cairo
    // 1.16, the same release as the tag API we already probe for.
    if (!title.empty())
        cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_TITLE,
                                       title.c_str());
#else
    (void)title;
#endif
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
        // the scale above maps it onto the PDF page. Heavy (textured) pages are
        // rasterised once to an embedded image so export time/size doesn't blow
        // up with the dot count; plain pages stay vector.
        if (page_is_heavy(n.pages[i])) {
            cairo_surface_t *img = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, (int)src_w, (int)src_h);
            if (img && cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
                cairo_t *ic = cairo_create(img);
                canvas_render_page(ic, n.pages[i], src_w, src_h);
                cairo_destroy(ic);
                cairo_set_source_surface(cr, img, 0, 0);
                cairo_paint(cr);
            } else {
                canvas_render_page(cr, n.pages[i], src_w, src_h);
            }
            if (img) cairo_surface_destroy(img);
        } else {
            canvas_render_page(cr, n.pages[i], src_w, src_h);
        }

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
