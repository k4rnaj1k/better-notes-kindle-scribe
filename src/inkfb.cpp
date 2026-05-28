#include "inkfb.h"

#if !BN_HAVE_FBINK

// FBInk not available (e.g. host build) — compile to no-op stubs so the
// app falls back to the GTK partial-redraw path.
namespace bn {
bool    inkfb_init(int)          { return false; }
bool    inkfb_available()        { return false; }
int     inkfb_screen_w()         { return 0; }
int     inkfb_screen_h()         { return 0; }
InkRect inkfb_draw_segment(double, double, double, double, double) {
    return InkRect{0, 0, 0, 0};
}
void    inkfb_settle(InkRect)    {}
void    inkfb_close()            {}
} // namespace bn

#else

#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <fbink.h>

// Direct-to-framebuffer ink via FBInk.
//
// We do NOT hand-roll the mxcfb ioctl any more: the Kindle Scribe is an MTK
// device and needs mxcfb_update_data_mtk + MXCFB_SEND_UPDATE_MTK + MTK
// waveform numbering. FBInk auto-detects the device and does the right
// thing, so we delegate the refresh to it. We draw the stroke with FBInk's
// rotation-aware rect fill (fbink_fill_rect_rgba) instead of poking raw fb
// memory, which also means FBInk handles the panel's native rotation.
//
// Coordinates: inkfb_draw_segment receives DRAWING-space (portrait) coords
// from the App. We map them to the X framebuffer's screen layout exactly
// like App::redraw_rect does, then hand FBInk *unrotated* fb coordinates
// (no_rota = true), because those screen coords are already in the raw fb
// layout the X server renders into.

namespace bn {

namespace {

int        g_fbfd = -1;
bool       g_ok = false;
int        g_rotation = 90;
int        g_screen_w = 0;     // raw fb (X landscape) dimensions
int        g_screen_h = 0;
FBInkConfig g_draw_cfg = {};   // for fills: black, no per-call refresh
FBInkConfig g_rfx_cfg  = {};   // for the explicit refresh: WFM_DU, fast

// drawing-space → raw fb (screen) point. Mirrors App::redraw_rect / the old
// inkfb drawing_to_screen_pt. Keep in sync with app.cpp.
void drawing_to_screen_pt(double dx, double dy, int &sx, int &sy) {
    switch (g_rotation) {
    case 90:  sx = g_screen_w - (int)dy; sy = (int)dx; break;
    case 180: sx = g_screen_w - (int)dx; sy = g_screen_h - (int)dy; break;
    case 270: sx = (int)dy;              sy = g_screen_h - (int)dx; break;
    default:  sx = (int)dx;              sy = (int)dy; break;
    }
}

void clampi(int &v, int lo, int hi) { v = std::max(lo, std::min(hi, v)); }

} // namespace

bool inkfb_init(int rotation_deg) {
    g_rotation = rotation_deg;

    g_fbfd = fbink_open();
    if (g_fbfd < 0) { log_err("inkfb: fbink_open failed"); return false; }

    // Fast B&W waveform for the live stroke. is_flashing=false keeps it a
    // PARTIAL update; no_refresh handled per-call.
    g_rfx_cfg = FBInkConfig{};
    g_rfx_cfg.wfm_mode    = WFM_DU;     // ~260ms full-quality DU; A2 is faster but B&W-only
    g_rfx_cfg.is_flashing = false;
    g_rfx_cfg.no_refresh  = false;

    if (fbink_init(g_fbfd, &g_rfx_cfg) < 0) {
        log_err("inkfb: fbink_init failed");
        fbink_close(g_fbfd); g_fbfd = -1; return false;
    }

    FBInkState st = {};
    fbink_get_state(&g_rfx_cfg, &st);
    g_screen_w = (int)st.screen_width;
    g_screen_h = (int)st.screen_height;
    log_info("inkfb: fbink ok  screen=%ux%u  bpp=%u  stride=%u  rota=%u",
             st.screen_width, st.screen_height, st.bpp,
             st.scanline_stride, st.current_rota);

    // Draw config: fill black, never refresh per-fill (we batch one refresh
    // per segment). We pass colour explicitly via fbink_fill_rect_rgba, so
    // fg/bg here are mostly irrelevant.
    g_draw_cfg = FBInkConfig{};
    g_draw_cfg.no_refresh = true;

    g_ok = true;
    return true;
}

bool inkfb_available() { return g_ok; }
int  inkfb_screen_w() { return g_ok ? g_screen_w : 0; }
int  inkfb_screen_h() { return g_ok ? g_screen_h : 0; }

InkRect inkfb_draw_segment(double x0, double y0,
                            double x1, double y1,
                            double width) {
    InkRect bbox{0, 0, 0, 0};
    if (!g_ok) return bbox;

    int sx0, sy0, sx1, sy1;
    drawing_to_screen_pt(x0, y0, sx0, sy0);
    drawing_to_screen_pt(x1, y1, sx1, sy1);
    int radius = std::max(1, (int)std::lround(width * 0.5));
    int diam   = radius * 2;

    // Walk the segment placing small black squares — a poor-man's brush
    // that approximates a round-capped line. Each fill is no_refresh; we
    // issue a single fast refresh over the union bbox at the end.
    int dx = std::abs(sx1 - sx0), sx = sx0 < sx1 ? 1 : -1;
    int dy = -std::abs(sy1 - sy0), sy = sy0 < sy1 ? 1 : -1;
    int err = dx + dy;
    int x = sx0, y = sy0;
    int step = std::max(1, radius);  // don't fill every pixel; overlap by radius
    int since = step;  // force a fill on the first point
    while (true) {
        if (since >= step || (x == sx1 && y == sy1)) {
            FBInkRect r;
            r.left   = (unsigned short)std::max(0, x - radius);
            r.top    = (unsigned short)std::max(0, y - radius);
            r.width  = (unsigned short)diam;
            r.height = (unsigned short)diam;
            // no_rota=true: coords are already in raw fb layout.
            fbink_fill_rect_rgba(g_fbfd, &g_draw_cfg, &r, true,
                                 0x00, 0x00, 0x00, 0xFF);
            since = 0;
        }
        if (x == sx1 && y == sy1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        ++since;
    }

    int minx = std::min(sx0, sx1) - radius;
    int miny = std::min(sy0, sy1) - radius;
    int maxx = std::max(sx0, sx1) + radius;
    int maxy = std::max(sy0, sy1) + radius;
    clampi(minx, 0, g_screen_w); clampi(maxx, 0, g_screen_w);
    clampi(miny, 0, g_screen_h); clampi(maxy, 0, g_screen_h);
    bbox.x = minx; bbox.y = miny;
    bbox.w = maxx - minx; bbox.h = maxy - miny;

    if (bbox.w > 0 && bbox.h > 0) {
        fbink_refresh(g_fbfd, (uint32_t)bbox.y, (uint32_t)bbox.x,
                      (uint32_t)bbox.w, (uint32_t)bbox.h, &g_rfx_cfg);
    }
    return bbox;
}

void inkfb_settle(InkRect r) {
    if (!g_ok || r.w <= 0 || r.h <= 0) return;
    // High-quality GC16 pass so the DU ink settles to clean grey.
    FBInkConfig gc = g_rfx_cfg;
    gc.wfm_mode = WFM_GC16;
    fbink_refresh(g_fbfd, (uint32_t)r.y, (uint32_t)r.x,
                  (uint32_t)r.w, (uint32_t)r.h, &gc);
}

void inkfb_close() {
    if (g_fbfd >= 0) { fbink_close(g_fbfd); g_fbfd = -1; }
    g_ok = false;
}

} // namespace bn

#endif // BN_HAVE_FBINK
