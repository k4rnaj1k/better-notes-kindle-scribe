#!/usr/bin/env bash
# Build leptonica + tesseract for the Kindle (arm-kindlehf) cross sysroot.
#
# Installs directly into the cross sysroot ($SYSROOT/usr). The cross
# pkg-config wrapper sets PKG_CONFIG_SYSROOT_DIR, which prepends $SYSROOT
# to every -I/-L from .pc files. Installing inside the sysroot makes
# those rewrites resolve correctly. Traineddata still lives in
# $HOME/x-tools-extra so the workflow can stage it without dipping back
# into the sysroot.
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

PREFIX="$SYSROOT/usr"
DATA_PREFIX="$HOME/x-tools-extra"
BUILD="$HOME/x-tools-build"
HOST="arm-${TARGET}-linux-gnueabihf"

mkdir -p "$PREFIX" "$DATA_PREFIX" "$BUILD"
cd "$BUILD"

# ---- leptonica ------------------------------------------------------------
if [ ! -f "$PREFIX/lib/pkgconfig/lept.pc" ]; then
  curl -fsSL "https://github.com/DanBloomberg/leptonica/releases/download/${LEPT_VER}/leptonica-${LEPT_VER}.tar.gz" \
    | tar xz
  pushd "leptonica-${LEPT_VER}"
    ./configure --host="$HOST" --prefix="$PREFIX" \
      --disable-programs --disable-shared --enable-static \
      --without-giflib --without-libwebp \
      --without-libopenjpeg --without-libtiff --without-jpeg
    make -j"$(nproc)"
    sudo env PATH="$PATH" make install
  popd
fi

# ---- tesseract ------------------------------------------------------------
if [ ! -f "$PREFIX/lib/pkgconfig/tesseract.pc" ]; then
  curl -fsSL "https://github.com/tesseract-ocr/tesseract/archive/refs/tags/${TESS_VER}.tar.gz" \
    | tar xz
  pushd "tesseract-${TESS_VER}"
    ./autogen.sh
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
    LDFLAGS="-static-libstdc++ -static-libgcc" \
    ac_cv_header_archive_h=no \
    ac_cv_header_curl_curl_h=no \
      ./configure --host="$HOST" --prefix="$PREFIX" \
        --disable-graphics --disable-openmp --disable-shared --enable-static \
        --disable-doc
    make -j"$(nproc)" training-deps=no
    sudo env PATH="$PATH" make install
  popd
fi

# Hand the freshly installed OCR files back to the runner so the cache
# save/restore round-trip doesn't need sudo.
sudo chown -R "$(id -u):$(id -g)" \
  "$PREFIX/include/leptonica" \
  "$PREFIX/include/tesseract" \
  "$PREFIX/lib/pkgconfig/lept.pc" \
  "$PREFIX/lib/pkgconfig/tesseract.pc" 2>/dev/null || true
sudo find "$PREFIX/lib" -maxdepth 1 \
  \( -name 'libleptonica.*' -o -name 'libtesseract.*' \) \
  -exec chown "$(id -u):$(id -g)" {} +

# ---- english traineddata --------------------------------------------------
mkdir -p "$DATA_PREFIX/share/tessdata"
if [ ! -f "$DATA_PREFIX/share/tessdata/eng.traineddata" ]; then
  curl -fsSL -o "$DATA_PREFIX/share/tessdata/eng.traineddata" \
    "https://github.com/tesseract-ocr/tessdata_fast/raw/${TESSDATA_VER}/eng.traineddata"
fi
