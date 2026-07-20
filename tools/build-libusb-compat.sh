#!/bin/sh
# Build a minimally-patched libusb for the macOS .app bundle so a HackRF/RTL-SDR
# can be claimed WITHOUT root and WITHOUT the restricted com.apple.vm.device-access
# entitlement (which can't be used on an ad-hoc signature — it breaks launch).
#
# Root cause: on recent macOS, libusb's darwin_capture_claim_interface only takes
# the privileged "device capture" path when darwin_kernel_driver_active() reports
# a driver bound to the interface:
#
#     if (auto_detach_kernel_driver && darwin_kernel_driver_active(...)) {
#         darwin_detach_kernel_driver(...);   // needs root or the entitlement
#     }
#     return darwin_claim_interface(...);     // plain USBInterfaceOpen, no capture
#
# On newer macOS a generic IOUSBHost child shows up for the HackRF interface, so
# darwin_kernel_driver_active() returns 1, the detach is attempted, and without
# root it fails — so the device is seen but can't be claimed. (hackrf_info run
# from a terminal doesn't hit this.) There is no real kernel driver to detach for
# a HackRF/RTL-SDR, so we patch darwin_kernel_driver_active() to always report
# "no driver": the detach/capture is skipped and the normal USBInterfaceOpen
# claim (which needs neither root nor an entitlement) is used.
#
# Everything else — device enumeration, streaming — is stock libusb, unchanged.
# Same version as Homebrew's libusb => same Mach-O compatibility version, so it's
# a transparent drop-in for what dylibbundler bundled.
#
# Run once per machine/arch:  make libusb-compat
# `make app` then swaps the result into the bundle automatically.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO/third_party/libusb-compat"
OUT="$OUT_DIR/libusb-1.0.0.dylib"

VER="$(brew list --versions libusb 2>/dev/null | awk '{print $2}')"
[ -n "$VER" ] || VER=1.0.30

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "Downloading libusb $VER..."
curl -fsSL -o libusb.tar.bz2 \
  "https://github.com/libusb/libusb/releases/download/v$VER/libusb-$VER.tar.bz2"
tar xf libusb.tar.bz2
cd "libusb-$VER"

# Patch darwin_kernel_driver_active(): when a child (driver) is found, still
# report "no driver" (return 0) so the capture/detach path is never taken.
SRC=libusb/os/darwin_usb.c
if grep -q "IOObjectRelease (child);" "$SRC"; then
  perl -0pi -e 's{(IOObjectRelease \(child\);\n\s*)return 1;}{$1return 0; /* MacHPSDR: report no kernel driver so USB capture/detach (needs root) is skipped */}' "$SRC"
  echo "Patched $SRC (darwin_kernel_driver_active -> no driver)"
  grep -A1 "IOObjectRelease (child);" "$SRC" | grep -q "return 0" || { echo "ERROR: patch did not apply"; exit 1; }
else
  echo "NOTE: patch anchor not found in $SRC; building unmodified."
fi

echo "Building libusb $VER..."
./configure --disable-udev >/dev/null
make -j"$(sysctl -n hw.ncpu)" >/dev/null

DY=libusb/.libs/libusb-1.0.0.dylib
install_name_tool -id @executable_path/../Frameworks/libusb-1.0.0.dylib "$DY"
mkdir -p "$OUT_DIR"
cp "$DY" "$OUT"
codesign --force --sign - "$OUT" 2>/dev/null || true

COMPAT="$(otool -l "$OUT" | awk '/LC_ID_DYLIB/{f=1} f&&/compatibility version/{print $3; exit}')"
echo "Built $OUT (libusb $VER, kernel-driver-active patched, compat $COMPAT)"
