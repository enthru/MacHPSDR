#!/usr/bin/env bash
# Opt-in integration test against our null Soapy driver. No GUI or radio.
# SANITIZE=1 tools/run-soapy-rate-test.sh enables ASan and UBSan in the app code.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
out="$(mktemp -d "${TMPDIR:-/tmp}/soapy-rate-test.XXXXXX")"
trap 'rm -rf "$out"' EXIT
./tools/build-soapy-null.sh "$out/plugins"
prefix=/usr
if [[ "$(uname -s)" == Darwin ]]; then prefix="$(brew --prefix)"; fi
flags=(-O1 -g -std=gnu2x -Wno-deprecated-declarations -ffunction-sections -fdata-sections -DSOAPYSDR -DLIQUID -DSSTV
       -I"$prefix/include" -Isrc/core -Isrc/proto -Isrc/dsp -Isrc/audio
       -Isrc/ui -Isrc/decode -Iwdsp -Ift8_lib -Ihfdl_lib)
# pkg-config emits compiler flags, intentionally split into arguments.
read -r -a gtk_cflags <<< "$(pkg-config --cflags gtk4)"
read -r -a gtk_libs <<< "$(pkg-config --libs gtk4)"
if [[ "${SANITIZE:-0}" == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
if [[ "$(uname -s)" == Darwin ]]; then
  flags+=(-I"$prefix/opt/liquid-dsp/include")
  link=(-Wl,-dead_strip -L"$prefix/opt/liquid-dsp/lib")
else
  link=(-Wl,--gc-sections)
fi
"${CC:-cc}" "${flags[@]}" "${gtk_cflags[@]}" tools/soapy_rate_offline.c \
  src/core/transmitter.c src/core/log.c -Lwdsp -lwdsp -L"$prefix/lib" \
  -lSoapySDR -lliquid -lm "${gtk_libs[@]}" "${link[@]}" \
  -Wl,-rpath,"$root/wdsp" -o "$out/test"
SOAPY_SDR_PLUGIN_PATH="$out/plugins" "$out/test"
