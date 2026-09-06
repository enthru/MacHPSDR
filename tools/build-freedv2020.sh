#!/usr/bin/env bash
# Build the exact Codec 2 + LPCNet pair needed by FREEDV_MODE_2020 into the
# repository-local build/freedv prefix. Nothing is installed system-wide.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/build/freedv-src"
PREFIX="$ROOT/build/freedv"
LPCNET_SRC="$SRC/LPCNet"
CODEC2_SRC="$SRC/codec2"
LPCNET_BUILD="$SRC/lpcnet-build"
CODEC2_BUILD="$SRC/codec2-build"
MODEL=lpcnet_191005_v1.0.tgz
MODEL_SHA=509440924751fdd87ffaa5683ee3dddd937af5c833b9104ccce65d51614926c8
LPCNET_REV=c8e51ac5e2fe674849cb53e7da44689b572cc246
CODEC2_REV=06d4c11e699b0351765f10398abb4f663a984f36

mkdir -p "$SRC" "$PREFIX"

if [ ! -d "$LPCNET_SRC/.git" ]; then
  git clone https://github.com/drowe67/LPCNet.git "$LPCNET_SRC"
fi
if [ ! -d "$CODEC2_SRC/.git" ]; then
  git clone https://github.com/drowe67/codec2.git "$CODEC2_SRC"
fi
git -C "$LPCNET_SRC" fetch --depth 1 origin "$LPCNET_REV"
git -C "$LPCNET_SRC" checkout --detach "$LPCNET_REV"
git -C "$CODEC2_SRC" fetch --depth 1 origin "$CODEC2_REV"
git -C "$CODEC2_SRC" checkout --detach "$CODEC2_REV"

mkdir -p "$LPCNET_BUILD"
if [ ! -f "$LPCNET_BUILD/$MODEL" ]; then
  curl -fL "https://rowetel.com/downloads/deep/$MODEL" -o "$LPCNET_BUILD/$MODEL"
fi
if command -v sha256sum >/dev/null 2>&1; then
  GOT=$(sha256sum "$LPCNET_BUILD/$MODEL" | awk '{print $1}')
else
  GOT=$(shasum -a 256 "$LPCNET_BUILD/$MODEL" | awk '{print $1}')
fi
if [ "$GOT" != "$MODEL_SHA" ]; then
  echo "error: LPCNet model checksum mismatch" >&2
  exit 1
fi

CPU_ARGS=()
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
  # This older LPCNet release emits the ARM32-only -mfpu=neon on Apple Silicon.
  # arm64 already has NEON, so no explicit compiler flag is needed.
  CPU_ARGS=(-DAVX=OFF -DAVX2=OFF -DSSE=OFF -DNEON=OFF)
fi

cmake -S "$LPCNET_SRC" -B "$LPCNET_BUILD" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  "${CPU_ARGS[@]}"
cmake --build "$LPCNET_BUILD" --parallel

cmake -S "$CODEC2_SRC" -B "$CODEC2_BUILD" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DLPCNET_BUILD_DIR="$LPCNET_BUILD" \
  -DUNITTEST=OFF
cmake --build "$CODEC2_BUILD" --parallel
cmake --install "$LPCNET_BUILD"
cmake --install "$CODEC2_BUILD"

echo
echo "FreeDV 2020 backend installed in $PREFIX"
echo "Build MacHPSDR with: make FREEDV_INCLUDE=FREEDV"
