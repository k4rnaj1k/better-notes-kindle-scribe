#!/bin/sh
# launch.sh - start BetterNotes in fullscreen, suppress framework UI.
#
# Mirrors the freeze/thaw pattern from kindle-tablet/kindle/bin/tablet-mode.sh:
# disable Pillow + screensaver while the app is running, restore on exit.

MARKER="/tmp/betternotes-active"
EXT_DIR="/mnt/us/extensions/betternotes"
NOTES_DIR="/mnt/us/documents/betternotes"

mkdir -p "$NOTES_DIR"

if [ -f "$MARKER" ]; then
    echo "BetterNotes already running"
    exit 0
fi
echo "active" > "$MARKER"

# Freeze framework
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
