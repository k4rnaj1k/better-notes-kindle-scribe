#!/bin/sh
# launch.sh - start BetterNotes in fullscreen, suppress framework UI.
#
# Mirrors the freeze/thaw pattern from kindle-tablet/kindle/bin/tablet-mode.sh:
# disable Pillow + screensaver while the app is running, restore on exit.

MARKER="/tmp/betternotes-active"
EXT_DIR="/mnt/us/extensions/betternotes"
NOTES_DIR="/mnt/us/documents/betternotes"

mkdir -p "$NOTES_DIR"

# Kill any stale instance. A previous betternotes still holding the
# executable open causes "Text file busy" (ETXTBSY) on exec, and a
# leftover marker file from a crash would otherwise block relaunch.
pkill -f "$EXT_DIR/bin/betternotes" 2>/dev/null
# Wait (whole-second sleeps — busybox may lack fractional sleep) for the
# old process to die and release the executable's text segment. Escalate
# to SIGKILL if it's still around after a few seconds.
i=0
while pgrep -f "$EXT_DIR/bin/betternotes" >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 4 ]; then
        pkill -9 -f "$EXT_DIR/bin/betternotes" 2>/dev/null
    fi
    if [ "$i" -ge 6 ]; then
        break
    fi
    sleep 1
done
rm -f "$MARKER"
echo "active" > "$MARKER"

# Freeze framework. NOTE: we do NOT touch screen orientation here — the
# Kindle X server can't rotate (no XRandR), so betternotes rotates its own
# rendering in software (cairo). The old `eips -o U` call was both wrong
# (eips treats "U"/"-o" as bitmap filenames) and unnecessary.
lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null

"$EXT_DIR/bin/betternotes" \
    --notes-dir "$NOTES_DIR" \
    --tessdata "$EXT_DIR/data" &
APP_PID=$!

while [ -f "$MARKER" ] && kill -0 "$APP_PID" 2>/dev/null; do
    sleep 1
done

kill "$APP_PID" 2>/dev/null
rm -f "$MARKER"

# Thaw framework
lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
lipc-send-event com.lab126.hal.usbError cycled 2>/dev/null
