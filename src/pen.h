#pragma once
// Pen + touch input source. Reads /dev/input/eventX directly, decodes the
// kernel's input_event stream, and exposes samples + tool-button changes to
// the GTK main loop via a thread-safe queue.
//
// Lifted in spirit from kindle-tablet/kindle/gtk-ui/src/main.cpp:80 and :141,
// expanded to capture absolute X/Y/pressure rather than just BTN_TOOL_PEN
// proximity.

#include "util.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

namespace bn {

enum class PenButton { None, Pen, Rubber };

struct PenSample {
    int       x = 0;       // device units (raw)
    int       y = 0;
    int       pressure = 0;
    bool      down = false;
    PenButton tool = PenButton::None;
    uint32_t  t_ms = 0;
};

struct PenCalibration {
    int min_x = 0, max_x = 20967;   // Scribe Wacom defaults; auto-overridden
    int min_y = 0, max_y = 15725;
    int max_pressure = 4095;
    bool swap_xy = true;
    bool invert_x = false;
    bool invert_y = false;
};

// Starts the background pen reader thread. cb is invoked on the reader
// thread for every decoded sample; cb implementations must be thread-safe
// or post to the main loop (we use g_idle_add in app.cpp).
class PenReader {
public:
    using Callback = std::function<void(const PenSample &)>;

    bool start(Callback cb);
    void stop();

    const PenCalibration &calibration() const { return cal_; }

private:
    static void *thread_main(void *self);
    void run();

    pthread_t thread_;
    std::atomic<bool> running_{false};
    Callback cb_;
    PenCalibration cal_;
};

} // namespace bn
