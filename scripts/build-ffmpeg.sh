#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
prefix="$cache/install"
build="$cache/ffmpeg"
source_root="$root/experiments/decoder-20261016/ffmpeg-7.0.3"
pkg-config --exists x264
mkdir -p "$build" "$prefix"
cd "$build"
if [[ ! -f config.h || "$source_root/configure" -nt config.h || "$root/scripts/build-ffmpeg.sh" -nt config.h ]]; then
    "$source_root/configure" --prefix="$prefix" \
        --enable-shared --disable-static --enable-gpl --enable-libx264 --enable-rpath \
        --disable-autodetect --disable-everything --disable-doc --disable-ffplay \
        --disable-hwaccels --disable-avdevice \
        --enable-network --enable-pthreads \
        --enable-decoder=h264,hevc --enable-parser=h264,hevc \
        --enable-encoder=libx264 \
        --enable-demuxer=mov,mpegts,h264,hevc,flv,rtsp \
        --enable-muxer=mpegts,mp4,flv \
        --enable-protocol=file,pipe,udp,tcp,rtp,http,rtmp \
        --enable-filter=buffer,buffersink,null,scale,format \
        --extra-ldflags=-Wl,--build-id=sha1 --enable-debug=2
fi
make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install
