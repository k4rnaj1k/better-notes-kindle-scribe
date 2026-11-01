#pragma once
// Client for the kindle-hid-passthrough daemon's local HTTP API (the same one
// its BTManager app uses), so BetterNotes can scan for, pair, connect and
// forget Bluetooth keyboards without leaving the app.
//
// Every request runs on a short-lived worker thread and its reply is
// delivered back on the GLib main loop, so a slow or absent daemon never
// stalls drawing or input.

#include "json.h"

#include <functional>
#include <string>
#include <vector>

namespace bn {

struct BtDevice {
    std::string address;
    std::string name;
    std::string protocol;     // "ble" | "classic"
    int         rssi      = 0;
    bool        connected = false;   // has a live session
    bool        ready     = false;   // ...and its HID input node is up
};

struct BtStatus {
    bool running  = false;    // Bluetooth on (daemon not suspended by /stop)
    bool scanning = false;
    bool pairing  = false;
    std::vector<BtDevice> paired;    // devices.conf, joined with live sessions
};

struct BtReply {
    bool        reached = false;     // the daemon answered at all
    bool        ok      = false;     // ...and said "ok": true
    std::string error;
    json::Value body;
};

using BtCallback = std::function<void(const BtReply &)>;

// GET http://127.0.0.1:<port><path> (port 8321, or $BN_HID_API_PORT).
// cb runs on the main loop.
void bt_request(const std::string &path, BtCallback cb);

// Percent-encode a query parameter value.
std::string bt_query_escape(const std::string &s);

BtStatus              bt_parse_status(const json::Value &v);
std::vector<BtDevice> bt_parse_devices(const json::Value &v);   // "devices" array
// The daemon auto-accepts pairing, but a BLE keyboard may still want a
// passkey typed on it; the daemon only logs that ("Display PIN: 123456").
// Returns the most recent one in a /logs reply, or "".
std::string           bt_find_pin(const json::Value &logs);

// Where kindle-hid-passthrough lives ($BN_HID_DIR, default
// /mnt/us/kindle_hid_passthrough) and whether it's installed there.
std::string bt_install_dir();
bool        bt_installed();
// Start the daemon the way launch.sh does: the upstart job if installed,
// else the binary in the background.
void        bt_start_service();

} // namespace bn
