#!/usr/bin/env bash
# Build leptonica + tesseract for the Kindle (arm-kindlehf) cross sysroot.
# Installs into $HOME/x-tools-extra (mirrors the layout of x-tools).
#
# Required env (set by CI before invoking this script):
#   TARGET    e.g. kindlehf
#   PATH      contains $HOME/x-tools/arm-${TARGET}-linux-gnueabihf/bin
#   SYSROOT   absolute path to the cross sysroot
#
# Pin versions here so the cache key (a hash of this file) busts on bump.
set -euo pipefail

LEPT_VER=1.84.1
TESS_VER=5.3.4
TESSDATA_VER=4.1.0

PREFIX="$HOME/x-tools-extra"
BUILD="$HOME/x-tools-build"
HOST="arm-${TARGET}-linux-gnueabihf"

mkdir -p "$PREFIX" "$BUILD"
cd "$BUILD"

# ---- leptonica ------------------------------------------------------------
if [ ! -f "$PREFIX/lib/pkgconfig/lept.pc" ]; then
  curl -fsSL "https://github.com/DanBloomberg/leptonica/releases/download/${LEPT_VER}/leptonica-${LEPT_VER}.tar.gz" \
    | tar xz
  pushd "leptonica-${LEPT_VER}"
    ./configure --host="$HOST" --prefix="$PREFIX" \
      --disable-programs --without-giflib --without-libwebp \
      --without-libopenjpeg --without-libtiff
    make -j"$(nproc)"
    make install
  popd
fi

# ---- tesseract ------------------------------------------------------------
if [ ! -f "$PREFIX/lib/pkgconfig/tesseract.pc" ]; then
  curl -fsSL "https://github.com/tesseract-ocr/tesseract/archive/refs/tags/${TESS_VER}.tar.gz" \
    | tar xz
  pushd "tesseract-${TESS_VER}"
    ./autogen.sh
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
    LEPTONICA_CFLAGS="-I$PREFIX/include/leptonica" \
    LEPTONICA_LIBS="-L$PREFIX/lib -llept" \
      ./configure --host="$HOST" --prefix="$PREFIX" \
        --disable-graphics --disable-openmp --disable-shared --enable-static
    make -j"$(nproc)" training-deps=no
    make install
  popd
fi

# ---- english traineddata --------------------------------------------------
mkdir -p "$PREFIX/share/tessdata"
if [ ! -f "$PREFIX/share/tessdata/eng.traineddata" ]; then
  curl -fsSL -o "$PREFIX/share/tessdata/eng.traineddata" \
    "https://github.com/tesseract-ocr/tessdata_fast/raw/${TESSDATA_VER}/eng.traineddata"
fi
