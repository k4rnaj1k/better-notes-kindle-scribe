#include "inkfb.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace bn {

namespace {

// ---- NXP MXC ePDC ioctl interface (Lab126 driver) -----------------------
// The Kindle Scribe ships the imx_epdc_fb driver. Constants below are the
// stable subset used by koreader's blitbuffer and the kindle-tablet
// daemon. We deliberately use the v1 struct layout — it works on every
// Lab126 board from PW3 onward including Scribe.

struct mxcfb_rect {
    uint32_t top, left, width, height;
};

struct mxcfb_alt_buffer_data {
    uint32_t phys_addr;
    uint32_t width, height;
    mxcfb_rect alt_update_region;
};

struct mxcfb_update_data {
    mxcfb_rect update_region;
    uint32_t   waveform_mode;
    uint32_t   update_mode;
    uint32_t   update_marker;
    int        temp;
    unsigned   flags;
    mxcfb_alt_buffer_data alt_buffer_data;
};

#ifndef MXCFB_SEND_UPDATE
#define MXCFB_SEND_UPDATE _IOW('F', 0x2E, mxcfb_update_data)
#endif

// Waveform modes — A2 is the fast 2-bit "animation" mode, perfect for ink.
// GC16 is the high-quality 16-grey mode used when the stroke settles.
constexpr uint32_t WAVEFORM_A2     = 4;
constexpr uint32_t WAVEFORM_GC16   = 2;
constexpr uint32_t TEMP_USE_AMBIENT = 0x1000;
constexpr uint32_t UPDATE_MODE_PARTIAL = 0x0;

// ---- module state -------------------------------------------------------
struct FbState {
    int        fd = -1;
    uint8_t   *mem = nullptr;
    size_t     mem_len = 0;
    fb_var_screeninfo var{};
    fb_fix_screeninfo fix{};
    int        rotation_deg = 90;   // drawing → screen rotation
    bool       ok = false;
    uint32_t   marker = 1;
};

FbState g_fb;

// Map drawing-space → screen-space (the inverse of cairo's transform).
// Matches the math in App::redraw_rect — keep these in sync.
void drawing_to_screen_pt(double dx, double dy, int &sx, int &sy) {
    int xw = (int)g_fb.var.xres;
    int xh = (int)g_fb.var.yres;
    switch (g_fb.rotation_deg) {
    case 90:  sx = xw - (int)dy; sy = (int)dx; break;
    case 180: sx = xw - (int)dx; sy = xh - (int)dy; break;
    case 270: sx = (int)dy;      sy = xh - (int)dx; break;
    default:  sx = (int)dx;      sy = (int)dy; break;
    }
}

// Plot a single pixel as black (intensity 0) into the fb. Handles the
// common Kindle bpps: 8 (Y8), 16 (RGB565), 32 (XRGB8888).
void plot_pixel(int sx, int sy) {
    if (sx < 0 || sy < 0 ||
        sx >= (int)g_fb.var.xres || sy >= (int)g_fb.var.yres) return;
    size_t off = (size_t)sy * g_fb.fix.line_length +
                 (size_t)sx * (g_fb.var.bits_per_pixel / 8);
    if (off >= g_fb.mem_len) return;
    switch (g_fb.var.bits_per_pixel) {
    case 8:  g_fb.mem[off] = 0x00; break;
    case 16: *reinterpret_cast<uint16_t *>(g_fb.mem + off) = 0x0000; break;
    case 32: *reinterpret_cast<uint32_t *>(g_fb.mem + off) = 0xFF000000; break;
    }
}

// Bresenham line with a circular brush of `radius` for stroke width.
void rasterise_line(int x0, int y0, int x1, int y1, int radius) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                if (ox * ox + oy * oy <= radius * radius)
                    plot_pixel(x0 + ox, y0 + oy);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void send_update(const InkRect &r, uint32_t waveform) {
    if (!g_fb.ok || r.w <= 0 || r.h <= 0) return;
    mxcfb_update_data u{};
    u.update_region.left   = std::max(0, r.x);
    u.update_region.top    = std::max(0, r.y);
    u.update_region.width  = (uint32_t)r.w;
    u.update_region.height = (uint32_t)r.h;
    u.waveform_mode  = waveform;
    u.update_mode    = UPDATE_MODE_PARTIAL;
    u.update_marker  = g_fb.marker++;
    u.temp           = TEMP_USE_AMBIENT;
    u.flags          = 0;
    ioctl(g_fb.fd, MXCFB_SEND_UPDATE, &u);
}

} // namespace

bool inkfb_init(int rotation_deg) {
    g_fb.rotation_deg = rotation_deg;
    g_fb.fd = ::open("/dev/fb0", O_RDWR);
    if (g_fb.fd < 0) { log_err("inkfb: open /dev/fb0 failed"); return false; }
    if (ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &g_fb.var) != 0 ||
        ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &g_fb.fix) != 0) {
        log_err("inkfb: FBIOGET_*SCREENINFO failed");
        ::close(g_fb.fd); g_fb.fd = -1; return false;
    }
    g_fb.mem_len = (size_t)g_fb.fix.line_length * g_fb.var.yres;
    g_fb.mem = (uint8_t *)mmap(nullptr, g_fb.mem_len, PROT_READ | PROT_WRITE,
                                MAP_SHARED, g_fb.fd, 0);
    if (g_fb.mem == MAP_FAILED) {
        log_err("inkfb: mmap failed (len=%zu)", g_fb.mem_len);
        g_fb.mem = nullptr;
        ::close(g_fb.fd); g_fb.fd = -1;
        return false;
    }
    log_info("inkfb: ok  %dx%d  bpp=%u  stride=%u  rotation=%d",
             g_fb.var.xres, g_fb.var.yres, g_fb.var.bits_per_pixel,
             g_fb.fix.line_length, rotation_deg);
    g_fb.ok = true;
    return true;
}

bool inkfb_available() { return g_fb.ok; }
int  inkfb_screen_w() { return g_fb.ok ? (int)g_fb.var.xres : 0; }
int  inkfb_screen_h() { return g_fb.ok ? (int)g_fb.var.yres : 0; }

InkRect inkfb_draw_segment(double x0, double y0,
                            double x1, double y1,
                            double width) {
    InkRect bbox{0, 0, 0, 0};
    if (!g_fb.ok) return bbox;
    int sx0, sy0, sx1, sy1;
    drawing_to_screen_pt(x0, y0, sx0, sy0);
    drawing_to_screen_pt(x1, y1, sx1, sy1);
    int radius = std::max(1, (int)std::round(width * 0.5));
    rasterise_line(sx0, sy0, sx1, sy1, radius);

    int minx = std::min(sx0, sx1) - radius - 1;
    int miny = std::min(sy0, sy1) - radius - 1;
    int maxx = std::max(sx0, sx1) + radius + 1;
    int maxy = std::max(sy0, sy1) + radius + 1;
    bbox.x = std::max(0, minx);
    bbox.y = std::max(0, miny);
    bbox.w = std::min((int)g_fb.var.xres, maxx) - bbox.x;
    bbox.h = std::min((int)g_fb.var.yres, maxy) - bbox.y;
    send_update(bbox, WAVEFORM_A2);
    return bbox;
}

void inkfb_settle(InkRect r) {
    send_update(r, WAVEFORM_GC16);
}

void inkfb_close() {
    if (g_fb.mem) { munmap(g_fb.mem, g_fb.mem_len); g_fb.mem = nullptr; }
    if (g_fb.fd >= 0) { ::close(g_fb.fd); g_fb.fd = -1; }
    g_fb.ok = false;
}

} // namespace bn
