#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
prefix="$cache/install"
pkg-config --exists x264 libevent libcurl
mkdir -p "$root/out/bin"
if [[ ${NAKI_SKIP_FFMPEG:-0} != 1 ]]; then
    bash "$root/scripts/build-ffmpeg.sh"
fi
python3 "$root/scripts/sdk-expected.py" verify --prefix "$prefix"
[[ ${NAKI_FFMPEG_ONLY:-0} == 1 ]] && exit 0
[[ -f "$root/third_party/yyjson/src/yyjson.c" ]] || { echo 'Run git submodule update --init --recursive' >&2; exit 1; }
cd "$root"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S "$root" -B "$cache/service" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$root/out/bin" -DNAKI_FFMPEG_PREFIX="$prefix" \
    -DNAKI_MEDIA_HOOK_SOURCE="${NAKI_MEDIA_HOOK_SOURCE:-}"
cmake --build "$cache/service" -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
printf 'Service: %s/out/bin/naki-live\nClient: %s/out/bin/naki-live-ctl\n' "$root" "$root"
