#!/bin/bash
#
# win-package.sh — assemble a runnable Windows folder around machpsdr.exe.
#
# Normally run from MSYS2 as `make win-package`.  A build tree produced by
# tools/win-crossbuild.sh can also be packaged from macOS/Linux by pointing
# MINGW_PREFIX at the cross sysroot and OBJDUMP at the cross objdump — which is
# the only way any of this gets exercised without a Windows machine.
#
#   MINGW_PREFIX   where the GTK runtime lives   (default: /mingw64)
#   OBJDUMP        PE reader                     (default: objdump)
#   BUILD_DIR      where machpsdr.exe was built  (default: the repo root)
#
# LAYOUT: everything is FLAT at the package root — the .exe, every DLL, then
# lib/ and share/ beside them.  This is not arbitrary.  glib derives its
# installation prefix at run time from the module's own directory, so with the
# binary at the root, <root>/lib/gdk-pixbuf-2.0 and <root>/share/glib-2.0 are
# exactly where it will look, and nothing needs an environment variable to be
# set before launch.  It also keeps assets/ next to the .exe, which is what
# win_startup() in main.c falls back to.

set -euo pipefail

DEST="${1:-machpsdr-win64}"
PREFIX="${MINGW_PREFIX:-/mingw64}"
OBJDUMP="${OBJDUMP:-objdump}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# The cross-build harness builds in a copy of the tree, not in the repo.
BUILD="${BUILD_DIR:-$REPO}"

[ -d "$PREFIX/bin" ] || { echo "error: no $PREFIX/bin — set MINGW_PREFIX"; exit 1; }
[ -f "$BUILD/machpsdr.exe" ] || { echo "error: no $BUILD/machpsdr.exe — build it first"; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST"

cp "$BUILD/machpsdr.exe" "$DEST/"
cp "$BUILD/wdsp/libwdsp.dll" "$DEST/" 2>/dev/null || cp "$BUILD/libwdsp.dll" "$DEST/"
cp -r "$REPO/assets" "$DEST/"

# ------------------------------------------------------------------- DLLs ---
# Walked transitively from the PE import tables rather than with ldd: this has
# to run on the cross setup too, where there is no Windows loader to ask.  Only
# DLLs found inside the prefix are copied — anything else (KERNEL32, WS2_32,
# the api-ms-win-* stubs) belongs to the system and must NOT be shipped.
copy_deps() {
  local file="$1" dll src
  while read -r dll; do
    [ -n "$dll" ] || continue
    [ -f "$DEST/$dll" ] && continue
    # bin/ AND lib/: most DLLs live in bin/, but not all — MSYS2 ships
    # libsoundio-2.dll in lib/, and missing it means the .exe does not start at
    # all ("Library libsoundio-2.dll not found", status c0000135).  Found by
    # running the package, not by reading it.
    src=""
    for dir in "$PREFIX/bin" "$PREFIX/lib"; do
      [ -f "$dir/$dll" ] && { src="$dir/$dll"; break; }
    done
    if [ -z "$src" ]; then
      # Case-insensitive retry: import tables spell names as the linker found
      # them, which does not always match the file on a case-sensitive host.
      for dir in "$PREFIX/bin" "$PREFIX/lib"; do
        src="$(find "$dir" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null || true)"
        [ -n "$src" ] && break
      done
    fi
    [ -n "$src" ] || continue
    cp "$src" "$DEST/"
    copy_deps "$src"
  done < <("$OBJDUMP" -p "$file" 2>/dev/null | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p')
}

echo "==> collecting DLLs"
copy_deps "$DEST/machpsdr.exe"
copy_deps "$DEST/libwdsp.dll"

# ----------------------------------------------------------- gdk-pixbuf ---
# The loaders are dlopen()ed, so they are invisible to the import walk above and
# have to be copied by hand — along with their own dependencies, which is how a
# package ends up able to show a window but not a PNG.
PIXBUF_VER="$(basename "$(find "$PREFIX/lib/gdk-pixbuf-2.0" -maxdepth 1 -mindepth 1 -type d | head -1)")"
if [ -n "$PIXBUF_VER" ]; then
  echo "==> gdk-pixbuf loaders ($PIXBUF_VER)"
  mkdir -p "$DEST/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders"
  cp "$PREFIX/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders/"*.dll \
     "$DEST/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders/"
  for l in "$DEST/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders/"*.dll; do
    copy_deps "$l"
  done

  # The cache names each loader by path.  Generated with the working directory
  # AT the package root so the paths come out relative, which is what makes the
  # folder movable; an absolute cache works only on the machine that built it.
  #
  # query-loaders works by dlopen()ing each module, so it must be the NATIVE
  # tool — run from macOS/Linux against PE modules it opens nothing and writes a
  # cache with no loaders in it.  That file is worse than no file: an empty
  # cache is not "fall back to defaults", it is "there are no loaders", and the
  # app then cannot load its icon or save a decoded image.  So the result is
  # checked and a useless cache is deleted rather than shipped.
  CACHE="$DEST/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders.cache"
  if command -v gdk-pixbuf-query-loaders >/dev/null; then
    ( cd "$DEST" && gdk-pixbuf-query-loaders \
        "lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders/"*.dll ) > "$CACHE" 2>/dev/null || true
  fi
  if [ ! -s "$CACHE" ] || ! grep -q '^"' "$CACHE" 2>/dev/null; then
    rm -f "$CACHE"
    echo "    ! loaders.cache NOT generated (needs the native gdk-pixbuf-query-loaders)."
    echo "      Run this script under MSYS2, or regenerate the cache there, or the"
    echo "      package will start and then fail to load or save any image."
  fi
fi

# --------------------------------------------------------------- schemas ---
# GTK reads its settings from the COMPILED schema file, which no package ships:
# it is built at install time.  Without it GTK aborts on startup.
echo "==> GSettings schemas"
mkdir -p "$DEST/share/glib-2.0/schemas"
cp "$PREFIX/share/glib-2.0/schemas/"*.xml "$DEST/share/glib-2.0/schemas/" 2>/dev/null || true
cp "$PREFIX/share/glib-2.0/schemas/gschema.dtd" "$DEST/share/glib-2.0/schemas/" 2>/dev/null || true
if command -v glib-compile-schemas >/dev/null; then
  glib-compile-schemas "$DEST/share/glib-2.0/schemas"
else
  echo "    ! glib-compile-schemas not found — gschemas.compiled NOT generated;"
  echo "      GTK will abort at startup."
fi

# ----------------------------------------------------------------- icons ---
# GTK4 draws its own widgets but still resolves named icons through the theme.
echo "==> icon themes"
mkdir -p "$DEST/share/icons"
for theme in Adwaita hicolor; do
  if [ -d "$PREFIX/share/icons/$theme" ]; then
    cp -r "$PREFIX/share/icons/$theme" "$DEST/share/icons/"
  fi
done

# ------------------------------------------------------------- SoapySDR ---
# Also dlopen()ed: without the modules the SoapySDR support links fine and finds
# no device at all, which reads as a broken build rather than a missing part.
# The drivers are SEPARATE MSYS2 packages from SoapySDR itself
# (mingw-w64-x86_64-soapyrtlsdr, -soapyhackrf); with none of them installed there
# is simply no module directory and this section is skipped.
SOAPY_MODDIR="$(find "$PREFIX/lib" -maxdepth 1 -type d -name 'SoapySDR' -print -quit || true)"
if [ -n "$SOAPY_MODDIR" ]; then
  echo "==> SoapySDR modules"
  cp -r "$SOAPY_MODDIR" "$DEST/lib/"
  while read -r m; do copy_deps "$m"; done < <(find "$DEST/lib/SoapySDR" -name '*.dll')
fi

echo
echo "==> $DEST"
du -sh "$DEST" 2>/dev/null || true
echo "    $(find "$DEST" -maxdepth 1 -name '*.dll' | wc -l | tr -d ' ') DLLs at the root"
