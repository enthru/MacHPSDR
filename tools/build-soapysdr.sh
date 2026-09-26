#!/bin/bash
#
# build-soapysdr.sh — build the SoapySDR CORE library for a native MSYS2/MinGW
# target and install the DLL, its import library and headers into PREFIX.
#
#   tools/build-soapysdr.sh <prefix> [version-tag]
#
# Why this exists: MSYS2 dropped the mingw64 (x86_64 GCC) build of SoapySDR --
# `mingw-w64-x86_64-soapysdr` is "target not found" now, the package survives
# only for the ucrt64/clang64 environments -- so the Windows CI can no longer
# `pacman -S` it. This rebuilds the same 0.8.x core the package shipped, into a
# prefix that is then copied into /mingw64 exactly as the liquid-dsp step does,
# so nothing downstream (the Makefile's -lSoapySDR, win-package.sh bundling
# /mingw64/bin/libSoapySDR.dll) has to change.
#
# Only the CORE library is built. The Windows package has never shipped a
# SoapySDR DEVICE driver (the release notes never claimed one), so this matches
# what was there: the API links and enumerates, and finds no devices, exactly as
# before. The Python/SWIG bindings and the tests are off -- the app needs
# neither, and they drag in interpreters this runner should not have to install.
#
# Pinned to a tag rather than master, same reasoning as build-liquid-dsp.sh: an
# unannounced upstream change must not arrive as a mystery failure of a tree
# that did not change. 0.8.1 is the version the removed MSYS2 package carried.

set -euo pipefail

PREFIX="${1:?usage: build-soapysdr.sh <prefix> [version-tag]}"
VER="${2:-soapy-sdr-0.8.1}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> SoapySDR $VER -> $PREFIX"

git clone --depth 1 --branch "$VER" -q \
    https://github.com/pothosware/SoapySDR.git "$WORK/src"

# "MSYS Makefiles" + an explicit compiler pins the build to the mingw64 gcc the
# rest of the job links against, rather than whatever else CMake might discover
# on the runner. Bindings and tests off: the app links only the core library.
cmake -S "$WORK/src" -B "$WORK/build" -G "MSYS Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_CXX_COMPILER=g++ \
      -DENABLE_PYTHON=OFF \
      -DENABLE_PYTHON3=OFF \
      -DENABLE_TESTS=OFF \
      -DENABLE_DOCS=OFF

cmake --build "$WORK/build" -j"$(nproc 2>/dev/null || echo 4)"
cmake --install "$WORK/build"

# Fail loudly if the pieces the downstream build needs are not where expected,
# rather than letting the next step's copy silently do nothing.
[ -f "$PREFIX/include/SoapySDR/Device.h" ] || {
  echo "error: SoapySDR headers were not installed"; exit 1; }
ls "$PREFIX/lib"/libSoapySDR.dll.a >/dev/null 2>&1 || {
  echo "error: libSoapySDR import library was not installed"; exit 1; }

echo "==> installed SoapySDR into $PREFIX"
