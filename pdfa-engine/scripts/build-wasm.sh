#!/usr/bin/env bash
set -euo pipefail

ENGINE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ENGINE/third_party"
QPDF_VERSION=12.3.2
OPENJPEG_VERSION=2.5.4
LCMS2_VERSION=2.16
EXPAT_VERSION=2.9.0
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

command -v emcmake >/dev/null 2>&1 || { echo "emscripten is not on PATH (emcmake not found)" >&2; exit 1; }
mkdir -p "$TP"

fetch() {
  local url=$1 dest=$2
  [ -f "$dest" ] && return 0
  echo "==> fetching $(basename "$dest")"
  curl -sSL --fail -o "$dest" "$url"
}

embuilder build zlib libjpeg freetype libpng >/dev/null
SYSROOT="$(em-config CACHE)/sysroot"
PORTS_INC="$SYSROOT/include"
PORTS_LIB="$SYSROOT/lib/wasm32-emscripten"

if [ ! -f "$TP/build-qpdf-wasm/libqpdf/libqpdf.a" ]; then
  fetch "https://github.com/qpdf/qpdf/releases/download/v${QPDF_VERSION}/qpdf-${QPDF_VERSION}.tar.gz" "$TP/qpdf.tar.gz"
  [ -d "$TP/qpdf-${QPDF_VERSION}" ] || tar -xzf "$TP/qpdf.tar.gz" -C "$TP"
  echo "==> building qpdf for wasm"
  emcmake cmake -S "$TP/qpdf-${QPDF_VERSION}" -B "$TP/build-qpdf-wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DREQUIRE_CRYPTO_NATIVE=ON -DUSE_IMPLICIT_CRYPTO=OFF \
    -DBUILD_DOC=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
    -DCMAKE_CXX_FLAGS=-fwasm-exceptions \
    -DZLIB_INCLUDE_DIR="$PORTS_INC" -DZLIB_LIBRARY="$PORTS_LIB/libz.a" \
    -DJPEG_INCLUDE_DIR="$PORTS_INC" -DJPEG_LIBRARY="$PORTS_LIB/libjpeg.a" >/dev/null
  cmake --build "$TP/build-qpdf-wasm" --target libqpdf -j"$JOBS"
fi

if [ ! -f "$TP/build-openjpeg-wasm/bin/libopenjp2.a" ]; then
  fetch "https://github.com/uclouvain/openjpeg/archive/refs/tags/v${OPENJPEG_VERSION}.tar.gz" "$TP/openjpeg.tar.gz"
  [ -d "$TP/openjpeg-${OPENJPEG_VERSION}" ] || tar -xzf "$TP/openjpeg.tar.gz" -C "$TP"
  echo "==> building openjpeg for wasm"
  emcmake cmake -S "$TP/openjpeg-${OPENJPEG_VERSION}" -B "$TP/build-openjpeg-wasm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF -DBUILD_DOC=OFF >/dev/null
  cmake --build "$TP/build-openjpeg-wasm" -j"$JOBS"
fi

if [ ! -f "$TP/build-lcms2-wasm/src/liblcms2.a" ]; then
  fetch "https://github.com/mm2/Little-CMS/archive/refs/tags/lcms${LCMS2_VERSION}.tar.gz" "$TP/lcms2.tar.gz"
  if [ ! -d "$TP/lcms2" ]; then
    tar -xzf "$TP/lcms2.tar.gz" -C "$TP"
    mv "$TP/Little-CMS-lcms${LCMS2_VERSION}" "$TP/lcms2"
  fi
  echo "==> building lcms2 for wasm"
  mkdir -p "$TP/build-lcms2-wasm/src"
  for src in "$TP"/lcms2/src/*.c; do
    emcc -O2 -I"$TP/lcms2/include" -c "$src" -o "$TP/build-lcms2-wasm/src/$(basename "${src%.c}").o"
  done
  emar rcs "$TP/build-lcms2-wasm/src/liblcms2.a" "$TP"/build-lcms2-wasm/src/*.o
fi

echo "==> building the engine"
if [ ! -f "$TP/install-expat-wasm/lib/libexpat.a" ]; then
  fetch "https://github.com/libexpat/libexpat/releases/download/R_2_9_0/expat-${EXPAT_VERSION}.tar.xz" "$TP/expat.tar.xz"
  printf '%s  %s\n' '1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051' "$TP/expat.tar.xz" | sha256sum -c -
  [ -d "$TP/expat-${EXPAT_VERSION}" ] || tar -xJf "$TP/expat.tar.xz" -C "$TP"
  emcmake cmake -S "$TP/expat-${EXPAT_VERSION}" -B "$TP/build-expat-wasm" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_DTD=OFF \
    -DCMAKE_INSTALL_PREFIX="$TP/install-expat-wasm"
  cmake --build "$TP/build-expat-wasm" -j"$JOBS"
  cmake --install "$TP/build-expat-wasm"
fi

emcmake cmake -S "$ENGINE" -B "$ENGINE/build-wasm" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPDFA_BUILD_CLI=OFF -DPDFA_BUILD_SDK=OFF -DPDFA_BUILD_FUZZ=OFF \
  -DEXPAT_INCLUDE_DIR="$TP/install-expat-wasm/include" \
  -DEXPAT_LIBRARY="$TP/install-expat-wasm/lib/libexpat.a" >/dev/null
cmake --build "$ENGINE/build-wasm" --target pdfa_wasm -j"$JOBS"

ls -la "$ENGINE/build-wasm/wasm/kura.js" "$ENGINE/build-wasm/wasm/kura.wasm"
if grep -q 'PDFA_WITH_PDFIUM:.*=ON' "$ENGINE/build-wasm/CMakeCache.txt"; then
  echo "==> built with PDFium: the module rasterizes"
else
  echo "==> built without PDFium: pages that need flattening are reported, not rasterized"
fi
