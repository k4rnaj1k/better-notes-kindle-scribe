#pragma once
#include "strokes.h"

namespace bn {

// A named drawing preset shown in the pen dropdown. `type` selects the render
// behaviour (Pencil tapers with pressure, Pen keeps a constant width) and
// `width` is the base stroke width in drawing units. New pen types are added
// simply by appending entries here.
struct PenPreset {
    const char* name;
    PenType     type;
    double      width;
};

inline const PenPreset kPenPresets[] = {
    { "Pencil", PenType::Pencil, 1.6 },  // pressure-tapered
    { "Pen",    PenType::Pen,    3.0 },  // constant medium
    { "Fine",   PenType::Pen,    1.8 },  // constant thin
    { "Bold",   PenType::Pen,    5.0 },  // constant thick
    { "Marker", PenType::Pen,    8.0 },  // constant very thick
};
constexpr int kPenPresetCount = (int)(sizeof(kPenPresets) / sizeof(kPenPresets[0]));

// Pressure→width taper floor: a Pencil thins to 40% at the lightest touch,
// a Pen holds full width (1.0 = no taper, so opacity/width no longer track
// press strength).
inline double pen_type_pressure_min(PenType t) {
    return t == PenType::Pencil ? 0.4 : 1.0;
}

struct ToolState {
    Tool   current          = Tool::Pen;
    int    pen_preset       = 0;       // index into kPenPresets
    double eraser_radius    = 30.0;
    bool   ocr_enabled      = false;
    bool   keyboard_visible = false;
};

const char *tool_label(Tool t);

} // namespace bn
