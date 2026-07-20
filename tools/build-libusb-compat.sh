#!/bin/sh
# Build a patched libusb for the macOS .app bundle so a HackRF/RTL-SDR can be
# claimed WITHOUT root and WITHOUT the restricted com.apple.vm.device-access
# entitlement (which can't be used on an ad-hoc signature — it breaks launch).
#
# Background: on recent macOS, stock libusb (>= 1.0.27) claims a USB interface via
# the new "device capture" path (darwin_capture_claim_interface), which requires
# root or that entitlement. It selects that path at compile time with
#   #if MAX_INTERFACE_VERSION >= 700  ->  capture claim
#   #else                            ->  legacy darwin_claim_interface (USBInterfaceOpen)
# The legacy claim needs neither root nor an entitlement, and it worked on macOS
# for years. Device *enumeration* is separate code and is unaffected.
#
# So we build the SAME libusb version the app otherwise bundles, but cap
# MAX_INTERFACE_VERSION at 550 so the legacy claim is compiled in. It is a
# transparent drop-in: same version => same Mach-O compatibility version, and we
# reset the install name to the bundle path.
#
# Run once per machine/arch:  make libusb-compat
# `make app` then swaps the result into the bundle automatically.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$REPO/third_party/libusb-compat"
OUT="$OUT_DIR/libusb-1.0.0.dylib"

# Build the same version Homebrew has, so the Mach-O compatibility version matches
# what libhackrf/the app were linked against (no version faking needed).
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

# Cap MAX_INTERFACE_VERSION at 550 -> legacy USBInterfaceOpen claim (no capture).
HDR=libusb/os/darwin_usb.h
if grep -q "set to the minimum version and casted up" "$HDR"; then
  perl -0pi -e 's{(/\* set to the minimum version and casted up as needed\. \*/)}{/* PATCH (MacHPSDR): force legacy USBInterfaceOpen claim, no macOS device-capture */\n#if defined(MAX_INTERFACE_VERSION) \&\& MAX_INTERFACE_VERSION > 550\n#undef MAX_INTERFACE_VERSION\n#define MAX_INTERFACE_VERSION 550\n#endif\n\n$1}' "$HDR"
  echo "Patched $HDR (MAX_INTERFACE_VERSION capped at 550)"
else
  echo "NOTE: patch anchor not found in $HDR; building unmodified (version may predate capture)."
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
echo "Built $OUT (libusb $VER, legacy-claim, compat $COMPAT)"
