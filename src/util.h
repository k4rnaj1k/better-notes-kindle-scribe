#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace bn {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

struct Point {
    double x = 0, y = 0;
    float  pressure = 1.0f;
    uint32_t t_ms   = 0;
};

void log_info(const char *fmt, ...);
void log_err (const char *fmt, ...);

// Read whole file into a string. Returns false if missing.
bool read_file(const std::string &path, std::string &out);
bool write_file(const std::string &path, const std::string &data);

bool ensure_dir(const std::string &path);
std::vector<std::string> list_dir(const std::string &path);
bool path_exists(const std::string &path);

// Recursively delete a file or directory. Returns true on success.
bool remove_path(const std::string &path);
// Move/rename a file or directory. Returns true on success.
bool rename_path(const std::string &from, const std::string &to);

// Slugify a title into a safe directory name.
std::string slugify(const std::string &title);

uint32_t now_ms();

// File modification time in ms (st_mtime * 1000), or 0 if the path is missing.
// Used by the file-watch poller to detect external (Syncthing) updates.
uint32_t file_mtime_ms(const std::string &path);

} // namespace bn
