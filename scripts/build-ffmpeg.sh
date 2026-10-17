#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
prefix="$cache/install"
build="$cache/ffmpeg"
source_root="$root/experiments/decoder-20261016/ffmpeg-7.0.3"
pkg-config --exists x264 srt zlib
mkdir -p "$build" "$prefix"
snapshot=$(mktemp "$build/.source-inputs.XXXXXX.json")
committed=0
cleanup() {
    rm -f -- "$snapshot" "$build/.naki-configure-key.pending"
    if [[ $committed != 1 ]]; then
        rm -f -- "$cache/.naki-object-keys.json" "$prefix/share/naki/ffmpeg-build.json" \
            "$build/.naki-object-key" "$build/.naki-object-key.pending" \
            "$cache/vvenc/.naki-object-key" "$cache/vvenc/.naki-object-key.pending"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 "$root/scripts/sdk-identity.py" snapshot --snapshot "$snapshot"
configure_key=$(python3 "$root/scripts/sdk-identity.py" configure-key)
object_key=$(python3 "$root/scripts/sdk-identity.py" object-key --snapshot "$snapshot" --component ffmpeg)
previous_key=$(python3 "$root/scripts/sdk-identity.py" cache-key --build-root "$cache" --prefix "$prefix" --component ffmpeg)
previous_vvenc_key=$(python3 "$root/scripts/sdk-identity.py" cache-key --build-root "$cache" --prefix "$prefix" --component vvenc)
python3 "$root/scripts/sdk-identity.py" invalidate-cache --build-root "$cache" --prefix "$prefix"
bash "$root/scripts/build-vvenc.sh" "$snapshot" "$previous_vvenc_key"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cd "$build"
reconfigure=0
if [[ ! -f config.h || ! -f ffbuild/config.mak || ! -f Makefile ||
      ! -f .naki-configure-key || $(cat .naki-configure-key) != "$configure_key" ]]; then
    reconfigure=1
    # Retire A before any cleanup/configure can mutate generated state to B.
    # A killed configure must force configuration again even after restoring A.
    rm -f -- .naki-configure-key .naki-configure-key.pending
fi
if [[ $reconfigure == 1 || $previous_key != "$object_key" ]]; then
    # Make timestamps are insufficient after source restoration/checkout. A
    # content change invalidates objects even when every source mtime is older.
    [[ ! -f Makefile ]] || make clean
fi
if [[ $reconfigure == 1 ]]; then
    "$source_root/configure" --prefix="$prefix" \
        --enable-shared --disable-static --enable-gpl --enable-libx264 --enable-libvvenc --enable-libsrt --enable-rpath \
        --disable-autodetect --disable-everything --disable-doc --disable-ffplay \
        --disable-hwaccels --disable-avdevice \
        --enable-network --enable-pthreads --enable-zlib \
        --enable-decoder=h264,hevc,vvc,aac,mp3,png,mjpeg,gif --enable-parser=h264,hevc,vvc,aac,mpegaudio,gif \
        --enable-bsf=aac_adtstoasc,extract_extradata \
        --enable-encoder=libx264,libvvenc \
        --enable-demuxer=mov,mpegts,h264,hevc,flv,rtsp,aac,mp3,png_pipe,jpeg_pipe,gif \
        --enable-muxer=mpegts,mp4,flv \
        --enable-protocol=file,pipe,udp,tcp,rtp,http,rtmp,libsrt \
        --enable-filter=buffer,buffersink,null,scale,format,overlay \
        --extra-ldflags=-Wl,--build-id=sha1 --enable-debug=2
    printf '%s\n' "$configure_key" > .naki-configure-key.pending
    mv -f -- .naki-configure-key.pending .naki-configure-key
fi
make -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
make install
printf '%s\n' "$object_key" > .naki-object-key.pending
python3 "$root/scripts/sdk-identity.py" seal --prefix "$prefix" --snapshot "$snapshot" --build-root "$cache"
committed=1
