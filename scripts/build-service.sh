#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
prefix="$cache/install"
service_build=${NAKI_SERVICE_BUILD_DIR:-$cache/service}
service_bin=${NAKI_SERVICE_BIN:-$root/out/bin}
pkg-config --exists x264 libevent libcurl
mkdir -p "$service_bin"
if [[ ${NAKI_SKIP_FFMPEG:-0} != 1 ]]; then
    bash "$root/scripts/build-ffmpeg.sh"
fi
python3 "$root/scripts/sdk-expected.py" verify --prefix "$prefix"
[[ ${NAKI_FFMPEG_ONLY:-0} == 1 ]] && exit 0
[[ -f "$root/third_party/yyjson/src/yyjson.c" ]] || { echo 'Run git submodule update --init --recursive' >&2; exit 1; }
cd "$root"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S "$root" -B "$service_build" -DCMAKE_BUILD_TYPE="${NAKI_BUILD_TYPE:-RelWithDebInfo}" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$service_bin" -DNAKI_FFMPEG_PREFIX="$prefix" \
    -DNAKI_SANITIZERS="${NAKI_SANITIZERS:-OFF}" \
    -DNAKI_MEDIA_HOOK_SOURCE="${NAKI_MEDIA_HOOK_SOURCE:-}"
cmake --build "$service_build" -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
printf 'Service: %s/naki-live\nClient: %s/naki-live-ctl\n' "$service_bin" "$service_bin"
