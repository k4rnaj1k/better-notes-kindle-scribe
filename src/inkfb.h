#pragma once
// Direct-to-framebuffer ink for the live (in-progress) stroke.
//
// Why: every pen sample going through X11 → GTK expose → cairo blit →
// e-ink controller adds ~150-500 ms of latency, because the X server uses
// the slow GC16 waveform by default. The stock Kindle Scribe note app
// feels instant because it writes directly to /dev/fb0 and asks the
// MXC e-ink driver for the A2 (animation) waveform — ~30-50 ms ghosting,
// monochrome only, perfect for ink strokes.
//
// This module:
//   * opens /dev/fb0, reads fb_var_screeninfo, mmaps the visible region
//   * exposes draw_segment(x0,y0,x1,y1,width) — rasterises a black line
//     into the framebuffer and issues MXCFB_SEND_UPDATE for its bbox
//   * exposes flush_area(rect) — final GC16 update so the strokes
//     "settle" cleanly when the pen lifts
//
// All coords are in *X-window* (screen) space — the App applies its
// drawing→screen transform before calling in. Set up the device with
// inkfb_init(rotation_deg); pass it 90 on the Scribe so the segments
// are rotated to match the physical layout.

#include <cstdint>

namespace bn {

struct InkRect { int x, y, w, h; };

bool inkfb_init(int rotation_deg);   // returns false if /dev/fb0 unusable
bool inkfb_available();
int  inkfb_screen_w();
int  inkfb_screen_h();

// Draw a line segment in *drawing-space* coordinates. Internally we transform
// to screen-space and rasterise into fb. Returns the screen-space bbox actually
// touched (so the caller can union them).
//
// `ink_level` is the target ink lightness 0=black .. 1=white. The panel under
// the DU waveform is 2-level, so grey is approximated by ordered dithering
// (sparser black pixels). 0 keeps the original solid-black behaviour exactly.
// `spray` scatters sparse dots within the brush radius instead of a solid disc
// (the spray-can pen), drawn directly to the framebuffer with no cairo resnap.
InkRect inkfb_draw_segment(double x0, double y0,
                            double x1, double y1,
                            double width, double ink_level = 0.0,
                            bool spray = false);

// Issue a high-quality (GC16) refresh over the union rect when the pen
// lifts, so the rough A2 ink "snaps" to clean greyscale. The rect is in
// *screen-space* (whatever inkfb_draw_segment returned).
void inkfb_settle(InkRect r);

void inkfb_close();

} // namespace bn
