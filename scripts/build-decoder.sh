#!/usr/bin/env bash
set -euo pipefail
if [[ -d /ucrt64/bin ]]; then export PATH="/ucrt64/bin:/usr/bin:$PATH"; fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
platform=${1:-linux}
case "$platform" in linux|windows) ;; *) echo 'Usage: build-decoder.sh [linux|windows]' >&2; exit 2;; esac
source_root="$root/experiments/decoder-20261016/ffmpeg-7.0.3"
[[ -f "$source_root/configure" ]] || { echo 'Initialize the FFmpeg submodule first.' >&2; exit 1; }
export FFMPEG_LIVE_BUILD_DIR="$root/out/build/$platform-auto"
export FFMPEG_LIVE_INSTALL_DIR="$root/out/install/$platform-auto"
script=build-linux.sh
[[ "$platform" == windows ]] && script=build-msys2.sh
exec bash "$source_root/tools/live-async/$script"
