#include "btclient.h"

#include <glib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>

namespace bn {

namespace {

int api_port() {
    const char *e = std::getenv("BN_HID_API_PORT");
    int p = e ? std::atoi(e) : 0;
    return p > 0 ? p : 8321;
}

// Blocking GET against localhost; fills reply.reached / body. The daemon
// closes the connection after each response, so read to EOF.
void http_get(const std::string &path, BtReply &reply) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { reply.error = "socket failed"; return; }
    // /remove and /disconnect wait on the daemon's event loop; give them room.
    timeval tv{6, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)api_port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        reply.error = "Bluetooth service isn't running";
        return;
    }

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n";
    for (size_t off = 0; off < req.size();) {
        ssize_t n = send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0) { close(fd); reply.error = "send failed"; return; }
        off += (size_t)n;
    }

    std::string raw;
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
    }
    close(fd);

    size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) { reply.error = "no response"; return; }
    reply.reached = true;
    std::string err;
    if (!json::parse(raw.substr(split + 4), reply.body, &err) ||
        reply.body.type != json::Type::Object) {
        reply.error = "bad reply";
        return;
    }
    auto *ok = reply.body.get("ok");
    reply.ok = ok && ok->type == json::Type::Bool && ok->b;
    if (!reply.ok) reply.error = reply.body.str("error", "failed");
}

struct Pending {
    BtCallback cb;
    BtReply    reply;
};

gboolean deliver(gpointer data) {
    auto *p = static_cast<Pending *>(data);
    p->cb(p->reply);
    delete p;
    return FALSE;
}

bool flag(const json::Value &v, const char *k) {
    auto *p = v.get(k);
    return p && p->type == json::Type::Bool && p->b;
}

BtDevice parse_device(const json::Value &d) {
    BtDevice dev;
    dev.address  = d.str("address");
    dev.name     = d.str("name");
    dev.protocol = d.str("protocol", "ble");
    dev.rssi     = (int)d.num("rssi", 0);
    return dev;
}

} // namespace

void bt_request(const std::string &path, BtCallback cb) {
    auto *p = new Pending{std::move(cb), {}};
    std::thread([p, path] {
        http_get(path, p->reply);
        g_idle_add(deliver, p);
    }).detach();
}

std::string bt_query_escape(const std::string &s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::vector<BtDevice> bt_parse_devices(const json::Value &v) {
    std::vector<BtDevice> out;
    auto *arr = v.get("devices");
    if (!arr || arr->type != json::Type::Array) return out;
    for (auto &d : arr->arr)
        if (d.type == json::Type::Object && !d.str("address").empty())
            out.push_back(parse_device(d));
    return out;
}

BtStatus bt_parse_status(const json::Value &v) {
    BtStatus st;
    st.running  = flag(v, "daemon_running");
    st.scanning = flag(v, "scanning");
    st.pairing  = flag(v, "pairing");
    st.paired   = bt_parse_devices(v);

    auto *conns = v.get("connections");
    if (conns && conns->type == json::Type::Array) {
        for (auto &c : conns->arr) {
            if (c.type != json::Type::Object) continue;
            std::string addr = c.str("address");
            for (auto &d : st.paired) {
                if (g_ascii_strcasecmp(d.address.c_str(), addr.c_str()) != 0) continue;
                d.connected = true;
                d.ready     = flag(c, "hid_ready");
                if (d.name.empty()) d.name = c.str("name");
            }
        }
    }
    return st;
}

std::string bt_find_pin(const json::Value &logs) {
    auto *lines = logs.get("lines");
    if (!lines || lines->type != json::Type::Array) return "";
    static const std::string kTag = "Display PIN: ";
    for (auto it = lines->arr.rbegin(); it != lines->arr.rend(); ++it) {
        if (it->type != json::Type::String) continue;
        size_t at = it->s.find(kTag);
        if (at == std::string::npos) continue;
        std::string pin;
        for (size_t i = at + kTag.size(); i < it->s.size() && g_ascii_isdigit(it->s[i]); ++i)
            pin += it->s[i];
        if (!pin.empty()) return pin;
    }
    return "";
}

std::string bt_install_dir() {
    const char *e = std::getenv("BN_HID_DIR");
    return e && *e ? e : "/mnt/us/kindle_hid_passthrough";
}

bool bt_installed() {
    struct stat st;
    return stat("/etc/upstart/hid-passthrough.conf", &st) == 0 ||
           stat((bt_install_dir() + "/kindle-hid-passthrough").c_str(), &st) == 0;
}

void bt_start_service() {
    std::string bin = bt_install_dir() + "/kindle-hid-passthrough";
    gchar *q = g_shell_quote(bin.c_str());
    std::string script =
        "if [ -f /etc/upstart/hid-passthrough.conf ]; then start hid-passthrough; "
        "else " + std::string(q) + " --daemon >/tmp/hid_passthrough.log 2>&1 & fi";
    g_free(q);
    gchar *argv[] = {(gchar *)"/bin/sh", (gchar *)"-c", (gchar *)script.c_str(), nullptr};
    auto flags = (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL);
    g_spawn_async(nullptr, argv, nullptr, flags, nullptr, nullptr, nullptr, nullptr);
}

} // namespace bn
