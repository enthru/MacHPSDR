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

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${1:-$ROOT/build/soapy-null}"
SRC="$HERE/soapy_null.cpp"

CXX_BIN="${CXX:-c++}"

# Homebrew's prefix must be asked for, not assumed: SoapySDR ships pkg-config, so
# use it when it answers and fall back to `brew --prefix` for the case where the
# .pc is not on the path (the same lesson as BREW_INCLUDES in the Makefile — a
# shell that has run `brew shellenv` hides the problem locally and CI finds it).
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
  Darwin) SHARED="-dynamiclib -undefined dynamic_lookup" ;;
  *)      SHARED="-shared" ;;
esac

mkdir -p "$OUT"
echo "==> $CXX_BIN -> $OUT/libmachpsdrnull.so"
# shellcheck disable=SC2086
"$CXX_BIN" -std=c++17 -O2 -fPIC -Wall -Wextra $SHARED \
  -o "$OUT/libmachpsdrnull.so" "$SRC" $SOAPY_CFLAGS $SOAPY_LIBS

echo "==> built $OUT/libmachpsdrnull.so"
echo "    SOAPY_SDR_PLUGIN_PATH=$OUT SoapySDRUtil --find"
