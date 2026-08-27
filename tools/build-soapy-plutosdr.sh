#!/bin/bash
#
# build-soapy-plutosdr.sh — build the PlutoSDR SoapySDR driver, and the two
# Analog Devices libraries under it, into a prefix `make app` can bundle:
#
#   tools/build-soapy-plutosdr.sh                 # -> build/soapy-plutosdr
#   tools/build-soapy-plutosdr.sh /tmp/somewhere  # -> that prefix instead
#
# WHY THIS EXISTS AT ALL.  HackRF and RTL-SDR arrive as `brew install
# soapyhackrf soapyrtlsdr`; Homebrew core has no PlutoSDR driver and no libiio
# either, and the tap that used to carry them is unmaintained.  So the one SDR
# this fork has done the most work for — every rate-substitution and coprime-
# resampler rule in CLAUDE.md came off a Pluto — is also the one that cannot be
# installed.  Building it here is what puts it in the .app beside the other two.
#
# It is NOT part of `make`.  Nothing links against libiio; this produces a
# dlopen'd module for another project's ABI, and an ordinary build of the
# application must not depend on a network fetch of three repositories.
# `make app` copies whatever it finds in the prefix below, so a tree that has
# never run this script simply ships two drivers instead of three.
#
# VERSIONS ARE PINNED, and the pin is the interesting part.  libiio's 1.x line
# is a different API; SoapyPlutoSDR 0.2.2 is written against 0.x, so v0.26 (the
# last of that line) is the version to build, not the newest tag.  Being pinned
# also means a CI cache can be keyed on this file: change a version here and the
# cache misses, which is the behaviour you want and not a coincidence.
IIO_TAG=v0.26
AD9361_TAG=v0.3
PLUTO_TAG=soapy-plutosdr-0.2.2
#
# libad9361-iio is optional to SoapyPlutoSDR's build and NOT optional to its
# usefulness here: it is what programmes the AD9361's FIR, and the FIR is how a
# Pluto reaches sample rates below the part's ~2.08 MHz floor.  Without it the
# driver quietly substitutes a rate eight times the one asked for — the exact
# fault CLAUDE.md describes as "looks like a bad LNB" — so a bundle built
# without this library ships the bug pre-installed.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PREFIX="${1:-$ROOT/build/soapy-plutosdr}"
WORK="$PREFIX/src"

command -v cmake >/dev/null 2>&1 || {
  echo "error: cmake not found (brew install cmake / apt-get install cmake)" >&2; exit 1; }
command -v git >/dev/null 2>&1 || {
  echo "error: git not found" >&2; exit 1; }

# SoapySDR itself is a build dependency of the driver, and unlike the null
# module this one cannot fall back to a bare -l: CMake has to find the config
# package.  Ask pkg-config first, brew second — the same order, and for the same
# reason, as tools/build-soapy-null.sh.
SOAPY_PREFIX=""
if pkg-config --exists SoapySDR 2>/dev/null; then
  SOAPY_PREFIX="$(pkg-config --variable=prefix SoapySDR)"
elif command -v brew >/dev/null 2>&1 && brew --prefix soapysdr >/dev/null 2>&1; then
  SOAPY_PREFIX="$(brew --prefix soapysdr)"
else
  echo "error: SoapySDR not found.  brew install soapysdr, or" >&2
  echo "       apt-get install libsoapysdr-dev" >&2
  exit 1
fi

EXTRA_PREFIX="$SOAPY_PREFIX"
if command -v brew >/dev/null 2>&1; then
  EXTRA_PREFIX="$EXTRA_PREFIX;$(brew --prefix)"
fi

mkdir -p "$WORK"

clone() {  # clone <url> <tag> <dir>
  local url="$1" tag="$2" dir="$3"
  if [ -d "$WORK/$dir/.git" ]; then
    echo "==> $dir already cloned"
    return
  fi
  echo "==> cloning $dir @ $tag"
  git clone --quiet --depth 1 --branch "$tag" "$url" "$WORK/$dir"
}

build() {  # build <dir> <extra cmake args...>
  local dir="$1"; shift
  echo "==> building $dir"
  # CMAKE_INSTALL_NAME_DIR: without it a dylib built here carries its build
  # directory as its install name, and dylibbundler then cannot rewrite what it
  # cannot resolve.  With it the reference is an absolute path in this prefix,
  # which resolves, gets copied into the bundle and gets relinked.
  cmake -S "$WORK/$dir" -B "$WORK/$dir/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" \
        -DCMAKE_PREFIX_PATH="$PREFIX;$EXTRA_PREFIX" \
        "$@" >/dev/null
  # TARGET=<name> builds that target instead of everything, which is how a
  # project whose *tests* do not compile still yields its library.  The install
  # step is unaffected: it installs what the install rules name, and no upstream
  # here installs its tests.
  if [ -n "${TARGET:-}" ]; then
    cmake --build "$WORK/$dir/build" --target "$TARGET" \
          --parallel "$(getconf _NPROCESSORS_ONLN)" >/dev/null
  else
    cmake --build "$WORK/$dir/build" --parallel "$(getconf _NPROCESSORS_ONLN)" >/dev/null
  fi
  cmake --install "$WORK/$dir/build" >/dev/null
}

clone https://github.com/analogdevicesinc/libiio.git        "$IIO_TAG"    libiio
clone https://github.com/analogdevicesinc/libad9361-iio.git "$AD9361_TAG" libad9361-iio
clone https://github.com/pothosware/SoapyPlutoSDR.git       "$PLUTO_TAG"  SoapyPlutoSDR

# OSX_FRAMEWORK=OFF is load-bearing: libiio's macOS build defaults to producing
# an iio.framework, which no consumer here looks for and dylibbundler does not
# handle.  The rest is trimming — this build exists to be dlopen'd by one
# application, so the daemon, the command-line tools, the tests, the bindings
# and the man pages are all things that would only be copied into a .app to sit
# there unused.
# INSTALL_UDEV_RULE=OFF is not trimming, it is the difference between building
# and not: on Linux libiio's install rule writes 90-libiio.rules to an ABSOLUTE
# /lib/udev/rules.d (UDEV_RULES_INSTALL_DIR, line 659 of its CMakeLists at this
# tag), ignoring CMAKE_INSTALL_PREFIX entirely -- so the install step fails with
# "Permission denied" as any non-root build must.  macOS has no udev and never
# reached it.
#
# What that rule does, and therefore what a bundle without it does not do: it
# grants a non-root user access to a Pluto attached over USB.  That is a
# property of the HOST, not of a relocatable bundle -- an AppImage cannot
# install into /lib and should not want to.  An operator using the Pluto over
# its USB-Ethernet address (ip:192.168.2.1, the usual way) needs nothing; one
# using a usb: context needs their distribution's libiio package, or the rule
# by hand.
build libiio \
  -DINSTALL_UDEV_RULE=OFF \
  -DOSX_FRAMEWORK=OFF \
  -DWITH_TESTS=OFF \
  -DWITH_DOC=OFF \
  -DWITH_IIOD=OFF \
  -DWITH_MAN=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_SERIAL_BACKEND=OFF \
  -DENABLE_PACKAGING=OFF \
  -DPYTHON_BINDINGS=OFF

# CMAKE_POLICY_VERSION_MINIMUM: libad9361-iio v0.3 declares
# cmake_minimum_required(VERSION 2.8), which CMake 4 refuses outright.  The flag
# is CMake's own escape hatch for exactly that, and it is preferable to the
# alternative of moving to v0.4.0 -- that release is written against libiio 1.x
# (`#include <iio/iio.h>`) and does not compile against the 0.x we need here.
# On a CMake old enough not to know the variable it is an unused-variable
# warning, so it costs nothing to pass unconditionally.
# TARGET=ad9361, because this project's TESTS do not build against the libiio
# we just installed and never will: they are written `#ifdef __APPLE__ ->
# #include <iio/iio.h>`, i.e. against the macOS FRAMEWORK layout, and the
# framework is precisely what OSX_FRAMEWORK=OFF above declined to build.  The
# library itself includes <iio.h> and compiles fine.  Building the one target
# is the narrow fix; the alternatives are patching upstream sources or shipping
# a framework nothing else here understands.
#
# OSX_PACKAGE defaults to ON and would have CMake assemble a .pkg installer --
# a macOS system installer, built as a side effect of a driver build, in a tree
# whose whole point is a self-contained .app.
TARGET=ad9361 build libad9361-iio \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOSX_PACKAGE=OFF \
  -DENABLE_PACKAGING=OFF

# libad9361 sets FRAMEWORK TRUE on its target unconditionally -- there is no
# option to turn it off -- so it installs lib/ad9361.framework and no plain
# library or header.  SoapyPlutoSDR's FindLibAD9361 then finds the framework as
# a "library" and no include directory at all, decides AD9361 support is
# unavailable, and builds without it: a driver that looks fine and cannot
# programme the FIR.  Turning the framework back into a dylib and a header
# afterwards costs three commands and touches no upstream source.  The framework
# is then removed rather than left beside them, because find_library() prefers a
# framework on macOS and would go straight back to the one that does not work.
AD9361_FW="$PREFIX/lib/ad9361.framework"
if [ -d "$AD9361_FW" ]; then
  echo "==> unwrapping ad9361.framework into lib/libad9361.dylib"
  mkdir -p "$PREFIX/include"
  cp "$AD9361_FW"/Headers/*.h "$PREFIX/include/"
  cp "$AD9361_FW"/Versions/Current/ad9361 "$PREFIX/lib/libad9361.dylib"
  install_name_tool -id "$PREFIX/lib/libad9361.dylib" "$PREFIX/lib/libad9361.dylib"
  rm -rf "$AD9361_FW"
fi

build SoapyPlutoSDR \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# The module is the whole point, and CMake will happily have installed headers
# and a static library while failing to produce it (a SoapySDR it could not find
# is the usual way).  The extension is .so on macOS too — SoapySDR builds its
# modules as CMake MODULE targets and scans for that suffix.
MODULE="$(find "$PREFIX/lib/SoapySDR" -name 'libPlutoSDRSupport.so' 2>/dev/null | head -1)"
[ -n "$MODULE" ] || {
  echo "error: built, but no libPlutoSDRSupport.so under $PREFIX/lib/SoapySDR" >&2
  exit 1
}

# Not decoration: the FIR is how a Pluto reaches a rate below ~2.08 MHz, and a
# driver built without libad9361 answers such a request by running eight times
# faster and saying nothing.  A silent downgrade is exactly the failure this
# script exists to prevent, so it is an error here rather than a surprise later.
# It is asked of whichever tool this platform has: `otool -L` on macOS, the
# dynamic section on Linux.  Written with otool alone it was a check that ran on
# ONE platform and silently passed on the other -- and the Linux AppImage is
# exactly where a Pluto operator would meet the substituted rate.
deps_of() {
  if command -v otool >/dev/null 2>&1; then
    otool -L "$1" 2>/dev/null
  elif command -v objdump >/dev/null 2>&1; then
    objdump -p "$1" 2>/dev/null | grep NEEDED
  elif command -v readelf >/dev/null 2>&1; then
    readelf -d "$1" 2>/dev/null | grep NEEDED
  else
    echo "__no_tool__"
  fi
}
DEPS="$(deps_of "$MODULE")"
if [ "$DEPS" = "__no_tool__" ]; then
  echo "warning: no otool/objdump/readelf here -- cannot check for libad9361" >&2
elif ! echo "$DEPS" | grep -q "libad9361"; then
  echo "error: the driver was built WITHOUT libad9361 -- it cannot programme" >&2
  echo "       the AD9361 FIR, so every rate below ~2.08 MHz will be" >&2
  echo "       substituted by the hardware, silently." >&2
  exit 1
fi

echo "==> built $MODULE"
echo "    SOAPY_SDR_PLUGIN_PATH=$(dirname "$MODULE") SoapySDRUtil --find"
