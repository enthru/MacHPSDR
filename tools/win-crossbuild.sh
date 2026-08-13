#!/bin/bash
#
# win-crossbuild.sh — compile the tree for Windows from a Linux or macOS box.
#
# This is a VERIFICATION harness, not a way to ship a build.  It exists because
# the Windows port cannot otherwise be compiled at all here: without it every
# line under _WIN32 is code that was read but never seen by a compiler, and the
# first pass of it found a dozen real errors that review had not (see the
# `feat(win): the tree now compiles and links for Windows` commit).
#
# It assembles a cross sysroot by unpacking MSYS2's own binary packages —
# dependency-resolved from the repository database — and points a mingw-w64
# cross-compiler at it.  What comes out is a genuine PE32+ machpsdr.exe and the
# offline harnesses `make check` runs, but nothing here RUNS any of them.  The
# .exe has been started under Wine, which answers "does it load and come up"
# and nothing about WASAPI, winmm, COM ports or real adapters
# — Wine reimplements exactly those.  For that you still need Windows.  The real
# build is `make` inside MSYS2, which the Makefile's MINGW branch is written for.
#
#   Prerequisites:  mingw-w64  (brew install mingw-w64 / apt install mingw-w64)
#                   zstd, curl, python3
#
#   Usage:  tools/win-crossbuild.sh [workdir]      (default: /tmp/machpsdr-win)
#
# HFDL needs liquid-dsp, which MSYS2 does not package.  If autoconf/automake are
# present it is built from source into the sysroot and HFDL is included; if not,
# HFDL is switched off and everything else still builds.

set -euo pipefail

WORK="${1:-/tmp/machpsdr-win}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
MIRROR="https://repo.msys2.org/mingw/mingw64"
HOST=x86_64-w64-mingw32

command -v "$HOST-gcc" >/dev/null || { echo "error: $HOST-gcc not found (install mingw-w64)"; exit 1; }
command -v zstd        >/dev/null || { echo "error: zstd not found"; exit 1; }

mkdir -p "$WORK"/{pkgs,sysroot,build}

# ---------------------------------------------------------------- sysroot ---
# Resolved transitively from mingw64.db rather than hardcoded: the package set
# drifts, and a stale list fails as a missing header three layers down.
if [ ! -f "$WORK/sysroot/mingw64/include/gtk-4.0/gtk/gtk.h" ]; then
  echo "==> fetching package database"
  curl -fsSL -o "$WORK/mingw64.db" "$MIRROR/mingw64.db"
  rm -rf "$WORK/dbx"; mkdir -p "$WORK/dbx"
  tar -xf "$WORK/mingw64.db" -C "$WORK/dbx"

  echo "==> resolving dependencies"
  python3 - "$WORK" <<'PY'
import os, re, sys
work = sys.argv[1]
root = os.path.join(work, 'dbx')
info = {}
for d in os.listdir(root):
    desc = os.path.join(root, d, 'desc')
    if not os.path.exists(desc):
        continue
    txt = open(desc, encoding='utf-8', errors='replace').read()
    def field(name):
        m = re.search(r'^%' + name + r'%\n(.*?)(?:\n\n|\Z)', txt, re.S | re.M)
        return m.group(1).split('\n') if m else []
    name = field('NAME')[0]
    info[name] = {
        'file': field('FILENAME')[0],
        'deps': [re.split(r'[<>=]', x)[0] for x in field('DEPENDS') if x.strip()],
        'provides': [re.split(r'[<>=]', x)[0] for x in field('PROVIDES') if x.strip()],
    }

# A package can be named by something it merely provides, so both map to it.
prov = {}
for n, v in info.items():
    prov[n] = n
    for p in v['provides']:
        prov.setdefault(p, n)

want = ['mingw-w64-x86_64-' + p for p in
        ('gtk4', 'fftw', 'libsoundio', 'soapysdr', 'zlib')]
seen, order, stack = set(), [], list(want)
while stack:
    real = prov.get(stack.pop())
    if real is None or real in seen:
        continue
    seen.add(real); order.append(real)
    stack.extend(info[real]['deps'])

with open(os.path.join(work, 'pkglist.txt'), 'w') as f:
    for n in sorted(order):
        f.write(info[n]['file'] + '\n')
print(f"    {len(order)} packages")
PY

  echo "==> downloading"
  while read -r f; do
    [ -f "$WORK/pkgs/$f" ] || curl -fsSL -o "$WORK/pkgs/$f" "$MIRROR/$f"
  done < "$WORK/pkglist.txt"

  echo "==> unpacking sysroot"
  for f in "$WORK"/pkgs/*.zst; do
    tar --use-compress-program=unzstd -xf "$f" -C "$WORK/sysroot" 2>/dev/null || true
  done
fi

# --------------------------------------------------------------- liquid ---
# Not an MSYS2 package; built from source by the shared script (which the CI
# workflow also uses).  Skipped without autotools — HFDL then goes off below.
LIQUID_VER=v1.7.0
if [ ! -f "$WORK/sysroot/mingw64/lib/libliquid.a" ] && command -v autoconf >/dev/null; then
  "$REPO/tools/build-liquid-dsp.sh" "$WORK/sysroot/mingw64" "$HOST" "$LIQUID_VER"
fi

# ------------------------------------------------------------------ build ---
# Built from a COPY of the tree: object files land in the repo root, so building
# in place would overwrite the host build's .o files with PE ones and leave the
# next native `make` linking a mixture.
echo "==> exporting tree"
rm -rf "$WORK/build"; mkdir -p "$WORK/build"
tar -c -C "$REPO" --exclude='*.o' --exclude='*.d' --exclude='.git' \
       --exclude='machpsdr' --exclude='*.dylib' --exclude='*.so' . \
  | tar -x -C "$WORK/build"

if [ -f "$WORK/sysroot/mingw64/lib/libliquid.a" ]; then
  # Point HFDL at the sysroot instead of the Makefile's `brew --prefix` lookup.
  sed -i.bak "s|^HFDL_INCLUDES=-I\$(shell brew --prefix liquid-dsp)/include \$(HFDL_VENDOR_INCLUDES)|HFDL_INCLUDES=\$(HFDL_VENDOR_INCLUDES)|" \
      "$WORK/build/Makefile"
else
  echo "==> no liquid-dsp (install autoconf/automake to build it) — HFDL off"
  sed -i.bak 's/^HFDL_INCLUDE=HFDL/#HFDL_INCLUDE=HFDL/' "$WORK/build/Makefile"
fi

echo "==> building"
export PKG_CONFIG_LIBDIR="$WORK/sysroot/mingw64/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$WORK/sysroot"

# UNAME_S is forced because the Makefile reads the HOST's uname; AR must be set
# too, or the sub-Makefiles (sgp4sdp4, hfdl_lib/asn1) archive PE objects with the
# host ar and produce an archive the cross linker cannot read.  WINDRES (the
# .exe icon + VERSIONINFO) would be derived from CC anyway, but it is named here
# so a toolchain whose gcc is not spelled `<host>-gcc` still finds it.
make -C "$WORK/build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
     UNAME_S=MINGW64_NT-10.0 \
     CC="$HOST-gcc" LINK="$HOST-gcc" AR="$HOST-ar" WINDRES="$HOST-windres"

# The offline harnesses, built but NOT run — nothing here can execute a PE.  They
# are worth cross-building for two reasons.  They link a different set of objects
# than the .exe does (no GTK symbols, no WDSP, no audio) with their own hand-
# written command lines, so a Windows link error can hide in a rule the app never
# reaches; and the MSYS2 CI workflow now RUNS them, which makes this the local
# gate on whether that step can even get as far as starting.  `check-build` is
# the flag-derived $(CHECK_BINS) list without the run, so a tree with HFDL off
# (which is what happens here when liquid-dsp could not be built) asks for
# exactly the harnesses it can link.
make -C "$WORK/build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
     UNAME_S=MINGW64_NT-10.0 \
     CC="$HOST-gcc" LINK="$HOST-gcc" AR="$HOST-ar" WINDRES="$HOST-windres" \
     check-build

echo
echo "==> $WORK/build/machpsdr.exe"
file "$WORK/build/machpsdr.exe"
# Subsystem 2 is GUI (no stray console on a double-click; the log still reaches
# a console the .exe was started from, via AttachConsole in win_startup), 3 is
# console — which is what `make WIN_CONSOLE=1` gives.  Printed because it is
# invisible in `file` output and is exactly the thing a packaging change breaks.
"$HOST-objdump" -p "$WORK/build/machpsdr.exe" | grep -i subsystem || true
"$HOST-objdump" -h "$WORK/build/machpsdr.exe" | grep -q '\.rsrc' \
  && echo "resources: .rsrc present (icon + VERSIONINFO)" \
  || echo "resources: NO .rsrc — the .exe has the default icon"

# Named individually rather than counted: "8 harnesses" tells nobody WHICH one a
# feature flag has quietly dropped.  Every one of these is a PE that has not been
# executed — the MSYS2 workflow is the only place they run.
echo
echo "==> harnesses (built, never run):"
for h in "$WORK"/build/*_offline.exe; do
  [ -e "$h" ] || { echo "    none — check-build produced nothing"; break; }
  echo "    $(basename "$h")"
done
