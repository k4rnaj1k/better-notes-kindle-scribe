# Stand-in for kindle-hid-passthrough's API on :8321: one paired keyboard,
# a 3 s scan that finds a few devices, a 4 s pairing that logs a passkey.
import json, threading, time
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, urlparse

S = {"running": True, "scanning": False, "scan_t": 0, "pairing": False, "pair": None,
     "pair_result": None, "scan_result": None, "logs": ["10:00:00 I daemon: up"],
     "devices": [{"address": "AA:BB:CC:00:00:01", "protocol": "ble", "name": "Logi K380"}],
     "connected": {"AA:BB:CC:00:00:01"}}
FOUND = [
    {"address": "AA:BB:CC:00:00:01", "name": "Logi K380", "protocol": "ble", "rssi": -50},
    {"address": "DE:AD:BE:EF:00:02", "name": "", "protocol": "ble", "rssi": -40},
    {"address": "DE:AD:BE:EF:00:03", "name": "Keychron K3", "protocol": "ble", "rssi": -62},
    {"address": "DE:AD:BE:EF:00:04", "name": "Galaxy Buds", "protocol": "classic", "rssi": -80},
]

def scan():
    S["scanning"] = True
    time.sleep(3)
    S["scanning"] = False
    S["scan_result"] = {"ok": True, "devices": FOUND}

def pair(addr, proto, name):
    time.sleep(1)
    S["logs"].append("10:00:05 I pairing: Display PIN: 482913")
    time.sleep(3)
    S["devices"].append({"address": addr, "protocol": proto, **({"name": name} if name else {})})
    S["connected"].add(addr)
    S["pairing"] = False
    S["pair_result"] = {"ok": True, "address": addr, "message": "Paired successfully"}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, d):
        b = json.dumps(d).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        print("GET", self.path, flush=True)
        p = u.path
        if p == "/status":
            self.send({"ok": True, "daemon_running": S["running"], "scanning": S["scanning"],
                       "pairing": S["pairing"], "devices": S["devices"],
                       "device_count": len(S["devices"]),
                       "connections": [{"address": a, "protocol": "ble", "name": "",
                                        "hid_ready": True} for a in S["connected"]]})
        elif p == "/scan":
            threading.Thread(target=scan, daemon=True).start()
            self.send({"ok": True, "message": "Scan started"})
        elif p == "/scan-status":
            if S["scanning"]: self.send({"ok": True, "scanning": True, "devices": FOUND[:2]})
            else: self.send(S["scan_result"] or {"ok": False, "error": "No scan in progress"})
        elif p == "/pair":
            S["pairing"] = True
            threading.Thread(target=pair, args=(q["addr"], q.get("protocol"), q.get("name")),
                             daemon=True).start()
            self.send({"ok": True})
        elif p == "/pair-status":
            self.send({"ok": True, "pairing": True} if S["pairing"] else S["pair_result"])
        elif p == "/logs":
            self.send({"ok": True, "lines": S["logs"][-40:]})
        elif p == "/connect":
            S["connected"].add(q["addr"]); self.send({"ok": True})
        elif p == "/disconnect":
            S["connected"].discard(q.get("addr")); self.send({"ok": True})
        elif p == "/remove":
            S["devices"] = [d for d in S["devices"] if d["address"] != q["addr"]]
            S["connected"].discard(q["addr"])
            self.send({"ok": True, "message": "Device removed"})
        elif p in ("/start", "/stop"):
            S["running"] = p == "/start"; self.send({"ok": True})
        else:
            self.send({"ok": False, "error": "Not found"})

class Srv(ThreadingMixIn, HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

Srv(("127.0.0.1", 8321), H).serve_forever()
