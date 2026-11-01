#!/bin/sh
# Host build + scripted UI session under Xvfb, with screenshots in /out.
# Runs inside the image from tests/ui/Dockerfile:
#   docker build -t bn-test tests/ui
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/ui-shots":/out bn-test sh /src/tests/ui/run.sh
#
# A fake Bluetooth keyboard comes in through fakeinput.so (an LD_PRELOAD shim
# standing in for /dev/input/eventN), and fake_hid_daemon.py stands in for
# kindle-hid-passthrough's API on :8321.
T=/src/tests/ui

cp -r /src /work && cd /work
meson setup b >/dev/null || exit 1
if ! ninja -C b >/tmp/build.log 2>&1; then
    grep -E "error|FAILED" /tmp/build.log | head -60
    exit 1
fi
echo BUILD_OK
gcc -shared -fPIC -o /tmp/fakeinput.so "$T/fakeinput.c" -ldl || exit 1

rm -rf /tmp/notes && mkdir -p /tmp/notes /dev/input
Xvfb :99 -screen 0 1404x1872x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99 BN_CONFIG=/tmp/bn.json
# The Bluetooth page's "Start service" runs this "kindle-hid-passthrough".
mkdir -p /tmp/hid
printf '#!/bin/sh\nexec python3 %s/fake_hid_daemon.py >/tmp/btd.log 2>&1\n' "$T" \
    > /tmp/hid/kindle-hid-passthrough
chmod +x /tmp/hid/kindle-hid-passthrough
export BN_HID_DIR=/tmp/hid

LD_PRELOAD=/tmp/fakeinput.so /work/b/betternotes --notes-dir /tmp/notes >/tmp/bn.log 2>&1 &
sleep 3
WID=$(xdotool search --onlyvisible --name "betternotes" | head -1)
xdotool windowsize --sync "$WID" 1404 1872
sleep 1
python3 "$T/session.py"
STATUS=$?

echo "--- files";  find /tmp/notes -maxdepth 1 | sort
echo "--- markdown"; for f in /tmp/notes/*.md; do echo "[$f]"; cat "$f"; echo "<EOF>"; done
echo "--- config"; cat /tmp/bn.json; echo
echo "--- log";    cat /tmp/bn.log
echo "--- fake daemon"; cat /tmp/btd.log 2>/dev/null
exit $STATUS
