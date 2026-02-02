#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT_DIR/userspace/zygisk_murasaki_bridge"
OUT_MOD_DIR="$ROOT_DIR/magisk-module"
OUT_ZYGISK_DIR="$OUT_MOD_DIR/zygisk"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is not set}"

case "$(uname -s)" in
  Darwin) PREBUILT="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64" ;;
  Linux)  PREBUILT="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64" ;;
  *) echo "Unsupported host OS"; exit 1 ;;
esac

build_one() {
  local abi="$1"
  local target="$2"
  local build_dir="$SRC_DIR/build-$abi"

  rm -rf "$build_dir"
  mkdir -p "$build_dir"

  cmake -S "$SRC_DIR" -B "$build_dir" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_ANDROID_ARCH_ABI="$abi" \
    -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
    -DANDROID_PLATFORM=android-29 \
    -DCMAKE_ANDROID_API=29 \
    -DCMAKE_C_COMPILER="$PREBUILT/bin/${target}-clang" \
    -DCMAKE_CXX_COMPILER="$PREBUILT/bin/${target}-clang++" \
    -DCMAKE_BUILD_TYPE=Release

  ninja -C "$build_dir"

  "$PREBUILT/bin/llvm-strip" -s "$build_dir/libmurasaki_zygisk_bridge.so"
  cp "$build_dir/libmurasaki_zygisk_bridge.so" "$OUT_ZYGISK_DIR/$abi.so"
}

mkdir -p "$OUT_ZYGISK_DIR"

build_one arm64-v8a aarch64-linux-android29
build_one armeabi-v7a armv7a-linux-androideabi29
build_one x86 i686-linux-android29
build_one x86_64 x86_64-linux-android29

(
  cd "$OUT_MOD_DIR"
  zip -r9 "../murasaki_bridge_zygisk.zip" . -x "zygisk/.gitkeep"
)

echo "Done: $ROOT_DIR/murasaki_bridge_zygisk.zip"

