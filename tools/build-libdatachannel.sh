#!/bin/bash
#
# build-libdatachannel.sh — build libdatachannel (and the usrsctp + libsrtp it
# needs for the SRTP media path) from source into a prefix the application links
# against and `make app` can bundle:
#
#   tools/build-libdatachannel.sh                 # -> build/libdatachannel
#   tools/build-libdatachannel.sh /tmp/somewhere  # -> that prefix instead
#
# WHY THIS EXISTS.  The browser remote's receive audio is WebRTC: a DTLS-SRTP
# Opus track the browser decodes with its own jitter buffer, so the server never
# ships I/Q or display-rate data over the Internet.  libdatachannel is the one
# native WebRTC stack with a C API (rtc/rtc.h) and macOS/Linux/Windows support;
# Google's libwebrtc is enormous and the others are Go/Python runtimes.  It is
# NOT in Homebrew or the mingw repos, so — exactly like the PlutoSDR driver — it
# is built from source here rather than vendored into the tree, and an ordinary
# `make` that has never run this script simply builds without WEBRTC.
#
# It is opt-in: the application links it only with `make WEBRTC_INCLUDE=WEBRTC`.
#
# LICENSE.  libdatachannel is MPL-2.0 (since 0.18), which is GPL-compatible; this
# tree is already effectively GPLv3 via the HFDL port.  usrsctp and libsrtp are
# BSD; the DTLS backend is OpenSSL (already a dependency here).
#
# VERSION IS PINNED, so a CI cache can key on this file: bump the tag and the
# cache misses, which is what you want.  The submodules (libjuice/usrsctp/
# libsrtp/plog/json) are taken at whatever the tag points to via --recursive.
DC_TAG="${DC_TAG:-v0.24.6}"

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PREFIX="${1:-$ROOT/build/libdatachannel}"
WORK="$PREFIX/src"

command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not found (brew install cmake / apt-get install cmake)" >&2; exit 1; }
command -v git >/dev/null 2>&1 || {
  echo "error: git not found" >&2; exit 1; }

# DTLS backend: OpenSSL.  CMake's FindOpenSSL does not look in Homebrew's
# keg-only prefix, so point it there when brew has it; on Linux/Windows the
# system/mingw OpenSSL is found normally.
OPENSSL_ARGS=()
if command -v brew >/dev/null 2>&1 && brew --prefix openssl >/dev/null 2>&1; then
  OPENSSL_ARGS=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl)")
fi

mkdir -p "$WORK"
if [ ! -d "$WORK/libdatachannel/.git" ]; then
  git clone --recursive --depth 1 --branch "$DC_TAG" \
      https://github.com/paullouisageneau/libdatachannel.git "$WORK/libdatachannel"
else
  git -C "$WORK/libdatachannel" fetch --depth 1 origin "$DC_TAG"
  git -C "$WORK/libdatachannel" checkout -f "$DC_TAG"
  git -C "$WORK/libdatachannel" submodule update --init --recursive --depth 1
fi

# NO_MEDIA=OFF keeps the SRTP media path (the Opus RTP track); NO_WEBSOCKET=ON
# because the application's control plane is its own WebSocket, not this one;
# shared libs so `make app` bundles one dylib per transitive library.
cmake -S "$WORK/libdatachannel" -B "$WORK/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=ON \
  -DNO_EXAMPLES=ON -DNO_TESTS=ON \
  -DNO_MEDIA=OFF -DNO_WEBSOCKET=ON \
  -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF \
  "${OPENSSL_ARGS[@]}"
cmake --build "$WORK/build" --parallel
cmake --install "$WORK/build"

echo
echo "libdatachannel installed into: $PREFIX"
echo "  headers: $PREFIX/include/rtc"
echo "  libs:    $PREFIX/lib"
echo "the next 'make' auto-detects it (force off with: make WEBRTC_INCLUDE=off)"
