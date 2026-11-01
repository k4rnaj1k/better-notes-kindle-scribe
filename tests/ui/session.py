# Fake Bluetooth keyboard session (through fakeinput.so): Ukrainian typing,
# layout switching, shortcuts, plus screenshots of every restyled screen.
import os, socket, struct, subprocess, time

EV_SYN, EV_KEY = 0, 1
KEY = {  # evdev codes
    'ESC': 1, 'BACKSPACE': 14, 'TAB': 15, 'ENTER': 28, 'LCTRL': 29, 'LSHIFT': 42,
    'LALT': 56, 'SPACE': 57, 'RALT': 100, 'F2': 60, 'HOME': 102, 'T_': 20,
    'UP': 103, 'DELETE': 111,
    'GRAVE': 41, 'MINUS': 12, 'COMMA': 51, 'DOT': 52, 'SLASH': 53,
    'Q': 16, 'W': 17, 'E': 18, 'R': 19, 'T': 20, 'Y': 21, 'U': 22, 'I': 23, 'O': 24,
    'P': 25, 'LBRACE': 26, 'RBRACE': 27, 'A': 30, 'S': 31, 'D': 32, 'F': 33, 'G': 34,
    'H': 35, 'J': 36, 'K': 37, 'L': 38, 'SEMI': 39, 'APOS': 40, 'BSLASH': 43,
    'Z': 44, 'X': 45, 'C': 46, 'V': 47, 'B': 48, 'N': 49, 'M': 50,
}
# Ukrainian character -> (US key, shift) it is typed with.
UA = {c: (k, False) for c, k in zip(
    "йцукенгшщзхїфівапролджєячсмитьбю",
    ['Q','W','E','R','T','Y','U','I','O','P','LBRACE','RBRACE',
     'A','S','D','F','G','H','J','K','L','SEMI','APOS',
     'Z','X','C','V','B','N','M','COMMA','DOT'])}
UA.update({"'": ('GRAVE', False), 'ґ': ('BSLASH', False), ' ': ('SPACE', False),
           '.': ('SLASH', False), ',': ('SLASH', True), '-': ('MINUS', False)})

SOCK = "/tmp/fakekbd.sock"
if os.path.exists(SOCK): os.unlink(SOCK)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(SOCK); srv.listen(4)
os.makedirs("/dev/input", exist_ok=True)
open("/dev/input/event7", "w").close()
srv.settimeout(5)
conn, _ = srv.accept()
print("keyboard connected", flush=True)

def ev(t, c, v): conn.sendall(struct.pack("llHHi", 0, 0, t, c, v))
def tap(*names, wait=0.12):
    for n in names: ev(EV_KEY, KEY[n], 1)
    ev(EV_SYN, 0, 0); time.sleep(0.03)
    for n in reversed(names): ev(EV_KEY, KEY[n], 0)
    ev(EV_SYN, 0, 0); time.sleep(wait)
def type_ua(text):
    for ch in text:
        key, shift = UA[ch.lower()]
        tap(*(['LSHIFT'] if shift or ch != ch.lower() else []), key)
def shot(name):
    time.sleep(0.8)
    subprocess.run(["import", "-window", "root", f"/out/{name}.png"])
    print("shot", name, flush=True)
def click(x, y):
    subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"])
    time.sleep(0.6)

time.sleep(0.5)
# --- markdown in Ukrainian -------------------------------------------------
tap('LCTRL', 'M', wait=1.0)
tap('LCTRL', 'SPACE')                                 # EN -> UA
type_ua("Привіт, світ. Їжак і м'ята, ")
tap('RALT', 'U'); type_ua("анок")                     # AltGr+г = ґ
tap('ENTER')
type_ua("- перший"); tap('ENTER'); type_ua("другий")
tap('LCTRL', 'S', wait=0.6)                           # US position in UA mode
shot("01_markdown_ua")
click(448, 49)                                        # toolbar: on-screen keyboard
shot("02_osk_ua")
tap('LALT', 'LSHIFT')                                 # Alt+Shift: UA -> EN
shot("03_osk_en")
click(448, 49)                                        # hide it again
tap('ESC', wait=1.0)                                  # back to browser

# --- notebook screens -------------------------------------------------------
tap('LCTRL', 'N', wait=1.2)
click(54, 49)                                          # pen menu
shot("04_pen_menu")
click(54, 49)
tap('LCTRL', 'T', wait=0.6)                            # template picker
shot("05_templates")
tap('ESC', wait=0.6)
tap('T_', wait=0.6)                                    # tags dialog
shot("06_tags")
tap('ESC', wait=0.6)
tap('ESC', wait=1.0)                                   # back to browser

# --- a folder with a Cyrillic name -----------------------------------------
tap('LCTRL', 'LSHIFT', 'N', wait=0.8)
tap('LCTRL', 'SPACE')                                  # EN -> UA
type_ua("Нотатки")
shot("07_new_folder_ua")
tap('ENTER', wait=1.0)
shot("08_browser")

# --- dialogs ----------------------------------------------------------------
tap('HOME'); tap('F2', wait=0.6)
shot("09_rename")
tap('ESC', wait=0.6)
click(726, 60)                                         # settings (gear)
shot("10_settings")

# --- Bluetooth page (fake daemon) ---------------------------------------------
click(959, 690)                                        # Settings: Bluetooth "Manage"
shot("11_bt_no_service")
click(702, 512)                                        # Start service
time.sleep(4)
shot("12_bt_paired")
click(1009, 432)                                       # Scan
time.sleep(1.2)
shot("13_bt_scanning")
time.sleep(4)
shot("14_bt_found")
click(1006, 520)                                       # Pair the first nearby device
time.sleep(2.5)
shot("15_bt_pairing_pin")
time.sleep(5)
shot("16_bt_paired_two")
tap('UP'); tap('DELETE', wait=1.5)                     # arm Forget on the K380
shot("17_bt_forget_armed")
tap('DELETE', wait=4)
shot("18_bt_forgot")
tap('ESC', wait=1.0)                                   # back to Settings
shot("19_settings_back")
tap('ESC', wait=0.6)

conn.close()
os.unlink("/dev/input/event7")
time.sleep(1.0)
print("done", flush=True)
