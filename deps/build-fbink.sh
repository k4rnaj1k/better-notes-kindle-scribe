#!/usr/bin/env bash
# Build FBInk (https://github.com/NiLuJe/FBInk) as a static library for the
# Kindle (arm-kindlehf) cross sysroot.
#
# FBInk is the canonical e-ink framebuffer library for Kindle/Kobo. We use
# it purely for the device-correct fast refresh path: on the Kindle Scribe
# (an MTK device) the live-ink fast refresh needs the mxcfb_update_data_mtk
# struct + MXCFB_SEND_UPDATE_MTK ioctl + MTK waveform numbering, which FBInk
# already implements and auto-detects at runtime. Rolling our own ioctl was
# how the first attempt silently no-op'd.
#
# Required env (set by CI before invoking this script):
#   TARGET    e.g. kindlehf
#   PATH      contains $HOME/x-tools/arm-${TARGET}-linux-gnueabihf/bin
#   SYSROOT   absolute path to the cross sysroot
#
# Pin the version here so the cache key (a hash of this file) busts on bump.
set -euo pipefail

FBINK_VER=v1.25.0

PREFIX="$SYSROOT/usr"
BUILD="$HOME/x-tools-build"
HOST="arm-${TARGET}-linux-gnueabihf"

mkdir -p "$PREFIX" "$BUILD"
cd "$BUILD"

if [ -f "$PREFIX/lib/libfbink.a" ] && [ -f "$PREFIX/include/fbink.h" ]; then
  echo "fbink: already installed"
  exit 0
fi

if [ ! -d FBInk ]; then
  git clone --branch "$FBINK_VER" --depth=1 --recursive \
    https://github.com/NiLuJe/FBInk.git
fi
cd FBInk

# FBInk's Makefile derives the toolchain from CROSS_TC. MINIMAL=1 drops the
# bundled fonts / image (stb) / OpenType code we don't need — we only call
# fbink_init / fbink_get_state / fbink_get_fb_pointer / fbink_fill_rect_rgba
# / fbink_refresh, all of which are core. KINDLE=1 enables the Kindle device
# quirks table (incl. the Scribe / MTK refresh path).
export CROSS_TC="$HOST"

# `staticlib` builds ONLY libfbink.a. The `static` target additionally
# links the CLI tool (fbink_cmd.c), which requires fixed-cell font support
# that MINIMAL=1 disables — that's the "Cannot build this tool without
# fixed-cell font rendering support" error. We only need the library.
make clean || true
make CROSS_TC="$HOST" KINDLE=1 MINIMAL=1 DEBUG=0 staticlib -j"$(nproc)"

# The static target produces Release/libfbink.a (+ the public header at root).
install -d "$PREFIX/lib" "$PREFIX/include"
# Locate the produced archive regardless of FBInk's output dir naming.
LIB="$(find . -name 'libfbink.a' -print -quit)"
if [ -z "$LIB" ]; then
  echo "fbink: libfbink.a not found after build" >&2
  exit 1
fi
sudo install -m 644 "$LIB" "$PREFIX/lib/libfbink.a"
sudo install -m 644 fbink.h "$PREFIX/include/fbink.h"

# Minimal pkg-config file so meson can find it via dependency('fbink').
sudo install -d "$PREFIX/lib/pkgconfig"
sudo tee "$PREFIX/lib/pkgconfig/fbink.pc" >/dev/null <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: fbink
Description: FrameBuffer eInker (static)
Version: ${FBINK_VER#v}
Libs: -L\${libdir} -lfbink
Libs.private: -lm
Cflags: -I\${includedir}
EOF

# Hand the installed files back to the runner so the cache round-trips
# without sudo (mirrors deps/build-tesseract.sh).
sudo chown -R "$(id -u):$(id -g)" \
  "$PREFIX/lib/libfbink.a" \
  "$PREFIX/include/fbink.h" \
  "$PREFIX/lib/pkgconfig/fbink.pc" 2>/dev/null || true

echo "fbink: installed to $PREFIX"
