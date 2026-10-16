#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode}
prefix="$cache/install"
build="$cache/ffmpeg"
source_root="$root/experiments/decoder-20261016/ffmpeg-7.0.3"
pkg-config --exists x264 libevent libcurl
mkdir -p "$build" "$prefix" "$root/out/bin"
cd "$build"
if [[ ! -f config.h || "$source_root/configure" -nt config.h || "$root/scripts/build-service.sh" -nt config.h ]]; then
    "$source_root/configure" --prefix="$prefix" \
        --enable-shared --disable-static --enable-gpl --enable-libx264 --enable-rpath \
        --disable-autodetect --disable-everything --disable-doc --disable-ffplay \
        --disable-hwaccels --disable-avdevice --disable-postproc \
        --enable-network --enable-pthreads \
        --enable-decoder=h264,hevc --enable-parser=h264,hevc \
        --enable-encoder=libx264 \
        --enable-demuxer=mov,mpegts,h264,hevc,flv,rtsp \
        --enable-muxer=mpegts,mp4,flv \
        --enable-protocol=file,pipe,udp,tcp,rtp,http,rtmp \
        --enable-filter=buffer,buffersink,null,scale,format \
        --enable-debug=2
fi
make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install
[[ ${NAKI_FFMPEG_ONLY:-0} == 1 ]] && exit 0
[[ -f "$root/third_party/yyjson/src/yyjson.c" ]] || { echo 'Run git submodule update --init --recursive' >&2; exit 1; }
cd "$root"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S "$root" -B "$cache/service" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$root/out/bin" -DNAKI_FFMPEG_PREFIX="$prefix"
cmake --build "$cache/service" -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
printf 'Service: %s/out/bin/naki-live\nClient: %s/out/bin/naki-live-ctl\n' "$root" "$root"
