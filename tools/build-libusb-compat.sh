#!/bin/sh
# Build a "capture-free" libusb for the macOS .app bundle.
#
# Why: recent libusb (>= 1.0.27) claims a USB interface on macOS via the new
# "device capture" API, which requires either root or the restricted
# com.apple.vm.device-access entitlement. An ad-hoc-signed .app has neither, so a
# bundled HackRF/RTL-SDR opens for descriptor reads but claim_interface fails and
# the device never enumerates (SoapySDR reports 0 devices). The restricted
# entitlement can't be used either — carrying it on an ad-hoc signature makes
# launchd refuse to start the app.
#
# libusb 1.0.26 (the last release before that change) claims the interface with
# the legacy IOKit USBInterfaceOpen path, which needs no entitlement and no root.
# This script builds it and relinks it as a drop-in for the newer libusb the app
# links against: same install name (@executable_path/../Frameworks/...) and the
# same Mach-O compatibility version as the system libusb (so dyld still accepts it
# under libhackrf, which was built against the newer one).
#
# Run once per machine/arch:  make libusb-compat   (or: sh tools/build-libusb-compat.sh)
# `make app` then swaps the result into the bundle automatically.
set -e

VER=1.0.26
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO/third_party/libusb-compat"
OUT="$OUT_DIR/libusb-1.0.0.dylib"

BREW="$(brew --prefix 2>/dev/null || echo /usr/local)"
SYS_LIBUSB="$BREW/lib/libusb-1.0.0.dylib"

# Match the compatibility version libhackrf/etc were linked against (= the system
# libusb the app would otherwise bundle), so the older lib still satisfies dyld.
COMPAT=""
if [ -f "$SYS_LIBUSB" ]; then
  COMPAT="$(otool -l "$SYS_LIBUSB" | awk '/LC_ID_DYLIB/{f=1} f&&/compatibility version/{print $3; exit}')"
fi
[ -n "$COMPAT" ] || COMPAT=6.0.0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "Downloading libusb $VER..."
curl -fsSL -o libusb.tar.bz2 \
  "https://github.com/libusb/libusb/releases/download/v$VER/libusb-$VER.tar.bz2"
tar xf libusb.tar.bz2
cd "libusb-$VER"

echo "Building libusb $VER..."
./configure --disable-udev >/dev/null
make -j"$(sysctl -n hw.ncpu)" >/dev/null

echo "Relinking as drop-in (install name + compatibility version $COMPAT)..."
mkdir -p "$OUT_DIR"
cc -dynamiclib -o "$OUT" \
   libusb/.libs/*.o libusb/os/.libs/*.o \
   -framework IOKit -framework CoreFoundation -framework Security -lobjc \
   -install_name @executable_path/../Frameworks/libusb-1.0.0.dylib \
   -compatibility_version "$COMPAT" -current_version "$COMPAT"
codesign --force --sign - "$OUT" 2>/dev/null || true

echo "Built $OUT (libusb $VER, compat $COMPAT)"
