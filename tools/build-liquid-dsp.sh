#!/bin/bash
#
# build-liquid-dsp.sh — build liquid-dsp for a MinGW target and install the
# static library plus its header into PREFIX.
#
#   tools/build-liquid-dsp.sh <prefix> [host-triplet] [version]
#
# Used by BOTH the MSYS2 CI workflow (native, no triplet) and
# tools/win-crossbuild.sh (cross, x86_64-w64-mingw32).  One copy on purpose:
# the three workarounds below were each found the hard way, and two copies of
# them would drift.
#
# liquid-dsp is the only dependency MSYS2 does not package, and the default-on
# HFDL decoder needs it.
#
# What upstream's configure assumes that MinGW does not have — none of which the
# library itself actually needs:
#
#   -lc              MinGW's C runtime is msvcrt/ucrt, so AC_CHECK_LIB(c, main)
#                    fails and configure stops at "Could not use standard C
#                    library".  Setting ac_cv_lib_c_main=yes is the obvious fix
#                    and the wrong one: it puts -lc into LIBS, and then every
#                    later check fails to link for THAT reason instead — the
#                    next one being -lm, which exists perfectly well.  An empty
#                    stub archive named libc.a on the link path is honest and
#                    costs nothing.
#
#   <sys/resource.h> configure stops at "Could not use standard headers".  Only
#                    liquid's bench/ sources use getrusage; the library does not.
#
#   libliquid.so     the shared-object rule emits a .so with an soname, which
#                    does not link on Windows.  libliquid.a is COMPLETE by the
#                    time that rule runs, so the archive is built as an explicit
#                    target and installed by hand.  `make install` is never
#                    reached and is not wanted.
#
# Because what lands is a static archive, whatever links it must also name
# liquid's own dependency: see -lfftw3f in the Makefile's MinGW branch.  An
# archive carries no dependencies of its own.

set -euo pipefail

PREFIX="${1:?usage: build-liquid-dsp.sh <prefix> [host] [version]}"
HOST="${2:-}"
VER="${3:-v1.7.0}"

# Pinned rather than tracking master: an unannounced upstream change would
# otherwise arrive as a mystery failure of a tree that did not change.

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$HOST" ]; then
  CC_BIN="$HOST-gcc"; AR_BIN="$HOST-ar"; HOSTFLAG="--host=$HOST"
else
  CC_BIN="${CC:-gcc}";  AR_BIN="${AR:-ar}"; HOSTFLAG=""
fi

echo "==> liquid-dsp $VER -> $PREFIX"

mkdir -p "$WORK/stub/lib" "$WORK/stub/include/sys"
: > "$WORK/stub/empty.c"
"$CC_BIN" -c "$WORK/stub/empty.c" -o "$WORK/stub/empty.o"
"$AR_BIN" rcs "$WORK/stub/lib/libc.a" "$WORK/stub/empty.o"
printf '#ifndef _STUB_SYS_RESOURCE_H\n#define _STUB_SYS_RESOURCE_H\n#endif\n' \
    > "$WORK/stub/include/sys/resource.h"

git clone --depth 1 --branch "$VER" -q \
    https://github.com/jgaeddert/liquid-dsp.git "$WORK/src"

cd "$WORK/src"
./bootstrap.sh >/dev/null 2>&1
# shellcheck disable=SC2086
./configure $HOSTFLAG --prefix="$PREFIX" \
            LDFLAGS="-L$WORK/stub/lib" CPPFLAGS="-I$WORK/stub/include" >/dev/null

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" libliquid.a

[ -f libliquid.a ] || { echo "error: libliquid.a was not built"; exit 1; }

mkdir -p "$PREFIX/include/liquid" "$PREFIX/lib"
cp include/liquid.h "$PREFIX/include/liquid/liquid.h"
cp libliquid.a      "$PREFIX/lib/"

echo "==> installed $PREFIX/lib/libliquid.a"
