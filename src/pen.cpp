#include "pen.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace bn {

namespace {

// Scan /sys/class/input for a pen digitizer device. Mirrors find_pen_device
// from kindle-tablet/kindle/gtk-ui/src/main.cpp:80.
int find_pen_fd(char path_out[256], int flags) {
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return -1;
    struct dirent *entry;
    int fd = -1;
    while ((entry = readdir(dir))) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        char name_path[300];
        std::snprintf(name_path, sizeof(name_path),
                      "/sys/class/input/%s/device/name", entry->d_name);
        FILE *f = std::fopen(name_path, "r");
        if (!f) continue;
        char name[128] = {0};
        if (std::fgets(name, sizeof(name), f)) {
            for (char *p = name; *p; ++p)
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            if (std::strstr(name, "wacom")     ||
                std::strstr(name, "stylus")    ||
                std::strstr(name, "ntx_event") ||
                std::strstr(name, "digitizer") ||
                std::strstr(name, "pen")) {
                std::fclose(f);
                char dev[256];
                std::snprintf(dev, sizeof(dev), "/dev/input/%s", entry->d_name);
                fd = open(dev, flags);
                if (path_out) std::strncpy(path_out, dev, 255);
                break;
            }
        }
        std::fclose(f);
    }
    closedir(dir);
    return fd;
}

void probe_calibration(int fd, PenCalibration &cal) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) {
        cal.min_x = ai.minimum; cal.max_x = ai.maximum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) {
        cal.min_y = ai.minimum; cal.max_y = ai.maximum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_PRESSURE), &ai) == 0) {
        cal.max_pressure = ai.maximum ? ai.maximum : 4095;
    }
}

} // namespace

bool PenReader::start(Callback cb) {
    cb_ = std::move(cb);
    running_ = true;
    if (pthread_create(&thread_, nullptr, &PenReader::thread_main, this) != 0) {
        running_ = false;
        return false;
    }
    pthread_detach(thread_);
    return true;
}

void PenReader::stop() {
    running_ = false;
}

void *PenReader::thread_main(void *self) {
    static_cast<PenReader *>(self)->run();
    return nullptr;
}

void PenReader::run() {
    char dev[256] = {0};
    int fd = find_pen_fd(dev, O_RDONLY);
    if (fd < 0) {
        log_err("pen: no digitizer device found");
        return;
    }
    probe_calibration(fd, cal_);
    log_info("pen: opened %s (x %d..%d y %d..%d p<=%d)",
             dev, cal_.min_x, cal_.max_x, cal_.min_y, cal_.max_y,
             cal_.max_pressure);

    PenSample cur{};
    bool dirty = false;
    struct input_event ev;
    while (running_) {
        ssize_t n = ::read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (errno == EAGAIN || errno == EINTR) continue;
            log_err("pen: read error: %s", std::strerror(errno));
            break;
        }
        switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
            case ABS_X:        cur.x = ev.value; dirty = true; break;
            case ABS_Y:        cur.y = ev.value; dirty = true; break;
            case ABS_PRESSURE: cur.pressure = ev.value; dirty = true; break;
            }
            break;
        case EV_KEY:
            switch (ev.code) {
            case BTN_TOUCH:       cur.down = ev.value != 0; dirty = true; break;
            case BTN_TOOL_PEN:    cur.tool = ev.value ? PenButton::Pen    : PenButton::None; dirty = true; break;
            case BTN_TOOL_RUBBER: cur.tool = ev.value ? PenButton::Rubber : PenButton::None; dirty = true; break;
            }
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT && dirty) {
                cur.t_ms = now_ms();
                if (cb_) cb_(cur);
                dirty = false;
            }
            break;
        }
    }
    ::close(fd);
}

} // namespace bn
