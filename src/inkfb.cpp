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
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fbink.h>

// Direct-to-framebuffer ink via FBInk's CORE api only.
//
// We don't hand-roll the mxcfb ioctl (the Scribe is MTK and needs the
// mxcfb_update_data_mtk struct / MXCFB_SEND_UPDATE_MTK / MTK waveform
// numbers — FBInk knows all that). A MINIMAL FBInk build strips the
// higher-level drawing helpers (fbink_fill_rect_rgba & friends), so we use
// only what's always present:
//   * fbink_get_fb_pointer() — the mmap'd /dev/fb0 (same buffer X renders
//     into), which we poke directly to set black pixels
//   * fbink_get_state()      — bpp / stride / geometry
//   * fbink_refresh()        — the device-correct fast partial refresh
//
// Coordinates: inkfb_draw_segment receives DRAWING-space (portrait) coords.
// We map them to the raw fb (X landscape) layout exactly like
// App::redraw_rect, write pixels there, and refresh that same region.

namespace bn {

namespace {

int        g_fbfd = -1;
bool       g_ok = false;
int        g_rotation = 90;
int        g_screen_w = 0;     // raw fb dimensions (X landscape on Scribe)
int        g_screen_h = 0;
uint32_t   g_bpp = 0;
uint32_t   g_stride = 0;       // bytes per scanline
uint8_t   *g_mem = nullptr;
size_t     g_mem_len = 0;
FBInkConfig g_rfx_cfg = {};    // refresh config: WFM_DU, partial

void drawing_to_screen_pt(double dx, double dy, int &sx, int &sy) {
    switch (g_rotation) {
    case 90:  sx = g_screen_w - (int)dy; sy = (int)dx; break;
    case 180: sx = g_screen_w - (int)dx; sy = g_screen_h - (int)dy; break;
    case 270: sx = (int)dy;              sy = g_screen_h - (int)dx; break;
    default:  sx = (int)dx;              sy = (int)dy; break;
    }
}

void clampi(int &v, int lo, int hi) { v = std::max(lo, std::min(hi, v)); }

// Set one pixel black. Handles the bpps the Kindle uses: 8 (Y8),
// 16 (RGB565), 32 (BGRA/XRGB).
inline void plot(int sx, int sy) {
    if (sx < 0 || sy < 0 || sx >= g_screen_w || sy >= g_screen_h) return;
    size_t off = (size_t)sy * g_stride + (size_t)sx * (g_bpp / 8);
    if (off + (g_bpp / 8) > g_mem_len) return;
    switch (g_bpp) {
    case 8:  g_mem[off] = 0x00; break;
    case 16: *reinterpret_cast<uint16_t *>(g_mem + off) = 0x0000; break;
    case 32: *reinterpret_cast<uint32_t *>(g_mem + off) = 0xFF000000u; break;
    default: g_mem[off] = 0x00; break;
    }
}

} // namespace

bool inkfb_init(int rotation_deg) {
    g_rotation = rotation_deg;

    g_fbfd = fbink_open();
    if (g_fbfd < 0) { log_err("inkfb: fbink_open failed"); return false; }

    g_rfx_cfg = FBInkConfig{};
    // Live-ink waveform. A2 is the fastest but it's a low-voltage *animation*
    // mode that renders a washed-out grey on this panel, so strokes looked
    // grey until the GC16 settle / cairo path snapped them to true black. DU
    // (Direct Update) is just as fast for the small per-segment regions but
    // drives pixels fully to black, matching the final render. Override with
    // BN_INK_WFM=A2|DU|DU4|GC16|GL16 to tune on-device without a rebuild.
    g_rfx_cfg.wfm_mode    = WFM_DU;
    if (const char *e = std::getenv("BN_INK_WFM")) {
        if      (!std::strcmp(e, "A2"))   g_rfx_cfg.wfm_mode = WFM_A2;
        else if (!std::strcmp(e, "DU"))   g_rfx_cfg.wfm_mode = WFM_DU;
        else if (!std::strcmp(e, "DU4"))  g_rfx_cfg.wfm_mode = WFM_DU4;
        else if (!std::strcmp(e, "GC16")) g_rfx_cfg.wfm_mode = WFM_GC16;
        else if (!std::strcmp(e, "GL16")) g_rfx_cfg.wfm_mode = WFM_GL16;
    }
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
    g_bpp      = st.bpp;
    g_stride   = st.scanline_stride;

    g_mem = fbink_get_fb_pointer(g_fbfd, &g_mem_len);
    if (!g_mem) {
        log_err("inkfb: fbink_get_fb_pointer failed");
        fbink_close(g_fbfd); g_fbfd = -1; return false;
    }

    log_info("inkfb: ok  screen=%dx%d  bpp=%u  stride=%u  rota=%u  fblen=%zu  wfm=%d",
             g_screen_w, g_screen_h, g_bpp, g_stride,
             (unsigned)st.current_rota, g_mem_len, (int)g_rfx_cfg.wfm_mode);
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

    // Bresenham with a round brush of `radius`.
    int dx = std::abs(sx1 - sx0), sx = sx0 < sx1 ? 1 : -1;
    int dy = -std::abs(sy1 - sy0), sy = sy0 < sy1 ? 1 : -1;
    int err = dx + dy;
    int x = sx0, y = sy0;
    while (true) {
        for (int oy = -radius; oy <= radius; ++oy)
            for (int ox = -radius; ox <= radius; ++ox)
                if (ox * ox + oy * oy <= radius * radius) plot(x + ox, y + oy);
        if (x == sx1 && y == sy1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
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
    g_mem = nullptr; g_mem_len = 0;
    g_ok = false;
}

} // namespace bn

#endif // BN_HAVE_FBINK
