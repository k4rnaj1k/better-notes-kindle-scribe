#include "util.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

namespace bn {

void log_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[betternotes] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void log_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[betternotes:ERR] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}

bool ensure_dir(const std::string &path) {
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> out;
    DIR *d = opendir(path.c_str());
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        out.emplace_back(e->d_name);
    }
    closedir(d);
    return out;
}

bool path_exists(const std::string &path) {
    struct stat st; return stat(path.c_str(), &st) == 0;
}

std::string slugify(const std::string &title) {
    std::string out; out.reserve(title.size());
    for (char c : title) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            out += c;
        else if (c >= 'A' && c <= 'Z')
            out += (char)(c + 32);
        else if (c == ' ' || c == '/')
            out += '-';
    }
    if (out.empty()) out = "untitled";
    return out;
}

uint32_t now_ms() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint32_t)(tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL);
}

} // namespace bn
