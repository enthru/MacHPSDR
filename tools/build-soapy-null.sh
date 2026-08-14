#!/bin/bash
#
# build-soapy-null.sh — build tools/soapy_null.cpp into a loadable SoapySDR
# module.  Opt-in, never installed:
#
#   tools/build-soapy-null.sh                 # -> build/soapy-null/libmachpsdrnull.so
#   tools/build-soapy-null.sh /tmp/somewhere  # -> that directory instead
#
# Then, and ONLY then, does anything see it:
#
#   SOAPY_SDR_PLUGIN_PATH=build/soapy-null SoapySDRUtil --find
#   SOAPY_SDR_PLUGIN_PATH=build/soapy-null SoapySDRUtil --probe=driver=machpsdrnull
#   SOAPY_SDR_PLUGIN_PATH=build/soapy-null HOME=$(mktemp -d) \
#       ./machpsdr --open machpsdrnull
#
# NOT in the Makefile, on purpose and twice over.  This is a C++ plugin against
# another project's ABI, in a C application whose build has no C++ in it at all;
# and it is a test fixture that must never be linked, installed or loaded by an
# ordinary build.  SOAPY_SDR_PLUGIN_PATH is *additive* — SoapySDR still loads the
# real drivers from its own modules directory — so pointing it here adds the null
# device to the list and takes nothing away.
#
# The extension is .so on every platform including macOS: SoapySDR builds its
# modules as CMake MODULE targets and looks for that suffix, so a .dylib here is
# simply never found and the failure looks like "the plugin does not register".
#
# NEVER BUILT WITH -fsanitize=..., even though the process that loads it is.  The
# CI job that uses this (.github/workflows/ci.yml, `sanitize`) runs an
# ASan+UBSan+LSan build of the app with SOAPY_SDR_PLUGIN_PATH pointing here, so
# the module is dlopen'd INTO an instrumented process — and that is fine
# uninstrumented, for the same reason CLAUDE.md gives for the vendored trees:
# ASan's allocator interception is process-wide, so every malloc/new this module
# makes still goes through ASan's allocator and LeakSanitizer still sees and
# reports whatever it forgets.  What instrumenting would add is redzones and UB
# checks on the FIXTURE, which is not the code under test; what it would cost is
# a fixture that can only be loaded by a sanitised process (SoapySDRUtil, or an
# ordinary ./machpsdr, would then need the runtime preloaded) and a build that
# has to know how the app was configured.  So: no sanitiser flags here, ever.
#
# -g -fno-omit-frame-pointer are here for the other half of that decision.  ASan
# is run with the fast unwinder (the CI job explains why), which walks frame
# pointers — so an -O2 -fomit-frame-pointer module in the middle of an allocation
# stack truncates the report to "<unknown module>" exactly when a leak on the
# SoapySDR path is what you are trying to read.  Two flags, no measurable cost on
# a source that spends its time in sin/cos.  -g is Linux-only, though: clang
# links a -dynamiclib -g build by running dsymutil, which drops a
# libmachpsdrnull.so.dSYM DIRECTORY next to the module — inside the one directory
# SOAPY_SDR_PLUGIN_PATH tells SoapySDR to scan for modules.  There is no
# LeakSanitizer on macOS for the symbols to serve, so the flag buys nothing there
# and costs a stray entry in the plugin path.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${1:-$ROOT/build/soapy-null}"
SRC="$HERE/soapy_null.cpp"

CXX_BIN="${CXX:-c++}"
command -v "$CXX_BIN" >/dev/null 2>&1 || {
  echo "error: no C++ compiler ($CXX_BIN).  This tree is C, so one is not" >&2
  echo "       implied by being able to build it: apt-get install g++, or" >&2
  echo "       set CXX." >&2
  exit 1
}

# Ask, never assume.  SoapySDR ships a pkg-config file on every platform that
# packages it (Debian/Ubuntu's libsoapysdr-dev and Homebrew's soapysdr both), so
# that is the first question; `brew --prefix` is the fallback for the macOS case
# where the .pc is not on the path (the same lesson as BREW_INCLUDES in the
# Makefile — a shell that has run `brew shellenv` hides the problem locally and
# CI finds it), and a bare -lSoapySDR is the last resort for a prefix the
# compiler already searches, which is where Ubuntu lands if pkg-config itself is
# not installed.
SOAPY_CFLAGS=""
SOAPY_LIBS=""
if pkg-config --exists SoapySDR 2>/dev/null; then
  SOAPY_CFLAGS="$(pkg-config --cflags SoapySDR)"
  SOAPY_LIBS="$(pkg-config --libs SoapySDR)"
elif command -v brew >/dev/null 2>&1 && brew --prefix soapysdr >/dev/null 2>&1; then
  P="$(brew --prefix soapysdr)"
  SOAPY_CFLAGS="-I$P/include"
  SOAPY_LIBS="-L$P/lib -lSoapySDR"
else
  SOAPY_LIBS="-lSoapySDR"
fi

case "$(uname -s)" in
  Darwin) SHARED="-dynamiclib -undefined dynamic_lookup"; DEBUG="" ;;
  *)      SHARED="-shared";                               DEBUG="-g" ;;
esac

mkdir -p "$OUT"
echo "==> $CXX_BIN -> $OUT/libmachpsdrnull.so"
# shellcheck disable=SC2086
"$CXX_BIN" -std=c++17 -O2 $DEBUG -fno-omit-frame-pointer -fPIC -Wall -Wextra $SHARED \
  -o "$OUT/libmachpsdrnull.so" "$SRC" $SOAPY_CFLAGS $SOAPY_LIBS

# A compiler that exits 0 without writing the file would be news, but the whole
# point of this fixture is to be loaded by NAME out of a directory: an absent or
# empty module fails later as "the plugin does not register", which reads like a
# bug in the plugin rather than a build that did not happen.
[ -s "$OUT/libmachpsdrnull.so" ] || {
  echo "error: $CXX_BIN exited 0 but produced no module" >&2
  exit 1
}

echo "==> built $OUT/libmachpsdrnull.so"
echo "    SOAPY_SDR_PLUGIN_PATH=$OUT SoapySDRUtil --find"
