#!/usr/bin/env bash
#
# Build a self-contained Linux AppImage from an already-built ./machpsdr.
#
# This is the Linux twin of `make app` (macOS) and tools/win-package.sh
# (Windows): one file the operator downloads, makes executable and runs, with
# GTK4, WDSP, liquid-dsp, SoapySDR and its device drivers inside it and nothing
# to install.  It is a script rather than a Makefile target body for the same
# reason win-package.sh is -- it has to run real logic (dependency resolution,
# assertions) and a recipe of backslash-continued shell is where that goes to
# die.  `make appimage` calls it.
#
# ---------------------------------------------------------------------------
# WHAT AN APPIMAGE CAN AND CANNOT PROMISE, stated here because both halves get
# claimed wrongly:
#
#   * The glibc floor is the BUILD machine's.  Everything bundled here is linked
#     against the glibc of whatever built it, and glibc is not bundled (it never
#     can be -- it is the loader).  Built on Ubuntu 24.04 this AppImage requires
#     glibc >= 2.39: Ubuntu 24.04+, Fedora 40+, Debian 13.  It will NOT start on
#     Debian 12, Ubuntu 22.04 or Mint 21, and there is no fixing that here --
#     the tree needs GtkFileDialog (GTK 4.10+, radio_dialog.c) and those distros
#     ship GTK 4.6/4.8, so it cannot be built on them either.  Anything older
#     needs GTK built from source in an old container, which is a different
#     project.
#
#   * The graphics stack is deliberately NOT bundled.  libGL, libEGL, libdrm,
#     libX11 and the Mesa drivers must come from the host or GTK4's GSK renderer
#     talks to a driver that does not match the running kernel/GPU.  linuxdeploy
#     excludes them by default (the AppImage "excludelist"); do not override it.
#
#   * Nothing here says the result RUNS.  That is what the smoke test in ci.yml
#     is for, and even that only starts it under Xvfb with the fake device.
# ---------------------------------------------------------------------------
#
# Usage:  tools/build-appimage.sh [output.AppImage]
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

[ "$(uname -s)" = "Linux" ] || { echo "build-appimage.sh: Linux only (this is $(uname -s))"; exit 1; }
[ -x ./machpsdr ] || { echo "build-appimage.sh: ./machpsdr not built -- run make first"; exit 1; }

ARCH=$(uname -m)
VERSION=$(git describe --abbrev=0 --tags 2>/dev/null || echo unknown)
OUT=${1:-MacHPSDR-${VERSION}-${ARCH}.AppImage}

APPDIR=$ROOT/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/machpsdr" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/512x512/apps" \
         "$APPDIR/apprun-hooks"

echo "==> staging AppDir ($VERSION, $ARCH)"

# The binary, and libwdsp beside it.  machpsdr is linked with an $ORIGIN/wdsp
# rpath (Makefile RPATH_FLAGS), so the vendored library has to keep that exact
# relative position -- copying it to usr/lib instead would need the rpath
# patched, and patching a working rpath to get back where you started is how
# these bundles break.
install -m755 ./machpsdr          "$APPDIR/usr/bin/machpsdr"
mkdir -p                          "$APPDIR/usr/bin/wdsp"
install -m755 ./wdsp/libwdsp.so   "$APPDIR/usr/bin/wdsp/libwdsp.so"

# Resources.  resource_path.h probes <exe>/../share/machpsdr, which is exactly
# this directory once the AppImage is mounted at $APPDIR -- the /usr/share
# fallbacks in the same lookups name paths on the HOST and would miss.
for f in machpsdr.png machpsdr_icon.png machpsdr_small.png cty.dat coastline.bin; do
    [ -f "assets/$f" ] && install -m644 "assets/$f" "$APPDIR/usr/share/machpsdr/$f"
done

# The desktop entry and the themed icon it names.  Icon=machpsdr (a NAME, not a
# path) is what both freedesktop icon lookup and appimagetool want, and it is
# also the name main.c already passes to gtk_window_set_icon_name() -- which
# until this bundle existed resolved to nothing anywhere, because no install
# ever put an icon under that name.
install -m644 machpsdr.desktop "$APPDIR/usr/share/applications/machpsdr.desktop"
install -m644 assets/machpsdr_icon.png \
        "$APPDIR/usr/share/icons/hicolor/512x512/apps/machpsdr.png"

# SoapySDR's device drivers are dlopen'd, so NOTHING LINKS AGAINST THEM and no
# dependency walker can see them.  That is precisely how release 4.1's macOS
# bundle shipped libSoapySDR and not one driver: it built, it signed, it
# launched, and it found no radio on any machine that did not already have the
# drivers installed.  Copy them by hand, point SOAPY_SDR_PLUGIN_PATH at the copy
# (the hook below), and let linuxdeploy resolve THEIR dependencies -- libhackrf,
# librtlsdr, libusb, libiio, libad9361 -- with --deploy-deps-only.
SOAPY_MODS=$(ls -d /usr/lib/"$ARCH"-linux-gnu/SoapySDR/modules* /usr/lib/SoapySDR/modules* 2>/dev/null | head -1 || true)
if [ -n "$SOAPY_MODS" ] && ls "$SOAPY_MODS"/*.so >/dev/null 2>&1; then
    DEST=$APPDIR/usr/lib/SoapySDR/$(basename "$SOAPY_MODS")
    mkdir -p "$DEST"
    cp "$SOAPY_MODS"/*.so "$DEST"/
    echo "==> SoapySDR modules: $(ls "$DEST" | tr '\n' ' ')"
else
    # Not fatal: an HPSDR-only build has no use for them, exactly as on macOS.
    # ci.yml is where a RELEASE artifact is asserted to carry them by name.
    echo "==> WARNING: no SoapySDR driver modules found under /usr/lib*/SoapySDR"
    echo "             this AppImage will enumerate NO SoapySDR device on a machine"
    echo "             that does not have the drivers installed itself.  Fix with:"
    echo "               sudo apt-get install soapysdr-module-all"
fi

# An AppRun HOOK rather than a custom AppRun: linuxdeploy's own AppRun sources
# every apprun-hooks/*.sh, and the GTK plugin installs one of its own there
# (pixbuf loaders, GSettings schemas, GIO modules).  Replacing AppRun outright
# would silently drop that.
cat > "$APPDIR/apprun-hooks/machpsdr.sh" <<'HOOK'
# SoapySDR finds its drivers by dlopen along this path and nowhere else.
if [ -d "$APPDIR/usr/lib/SoapySDR" ]; then
    for d in "$APPDIR"/usr/lib/SoapySDR/modules*; do
        [ -d "$d" ] && export SOAPY_SDR_PLUGIN_PATH="$d"
    done
fi
# Deliberately NOT exported: MACHPSDR_CTY / MACHPSDR_COASTLINE.  Those are the
# operator's overrides; the bundled copies are found by resource_path.h looking
# beside the executable, and exporting them here would take the override away.
HOOK
chmod +x "$APPDIR/apprun-hooks/machpsdr.sh"

# ---------------------------------------------------------------------------
# linuxdeploy + the GTK plugin do the actual bundling.  Upstream publishes only
# a rolling `continuous` release -- there are no version tags to pin to, which
# is worth knowing rather than papering over: a break here can arrive without
# anything in this repository changing.
# ---------------------------------------------------------------------------
TOOLS=$ROOT/build/appimage-tools
mkdir -p "$TOOLS"
fetch() {   # url, destination
    [ -f "$2" ] || curl -fsSL -o "$2" "$1"
    chmod +x "$2"
}
BASE=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous
fetch "$BASE/linuxdeploy-$ARCH.AppImage" "$TOOLS/linuxdeploy"
fetch "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" \
      "$TOOLS/linuxdeploy-plugin-gtk.sh"

# A CI runner has no FUSE, so the tools cannot mount themselves; this makes them
# extract and run instead.  Harmless on a desktop that does have it.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
# The plugin reads this to decide what to deploy; without it it guesses from the
# binary and can miss the pixbuf loaders.
export DEPLOY_GTK_VERSION=4

DEPS_ONLY=()
for so in "$APPDIR"/usr/lib/SoapySDR/modules*/*.so; do
    [ -f "$so" ] && DEPS_ONLY+=(--deploy-deps-only="$so")
done

echo "==> linuxdeploy"
"$TOOLS/linuxdeploy" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/machpsdr" \
    --desktop-file "$APPDIR/usr/share/applications/machpsdr.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/machpsdr.png" \
    --library "$APPDIR/usr/bin/wdsp/libwdsp.so" \
    "${DEPS_ONLY[@]}" \
    --plugin gtk \
    --output appimage

# linuxdeploy names the file from the desktop entry; give it the name a release
# asset wants, which says the version and the architecture the way the macOS and
# Windows assets do.
built=$(ls -t ./*.AppImage | head -1)
[ "$built" = "./$OUT" ] || mv "$built" "$OUT"
chmod +x "$OUT"

echo "==> $OUT  ($(du -h "$OUT" | cut -f1))"
