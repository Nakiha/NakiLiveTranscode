#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
mkdir -p "$cache/vvenc"
snapshot=${1:-}
own_snapshot=0
completed=0
cleanup() {
    [[ $own_snapshot != 1 ]] || rm -f -- "$snapshot"
    if [[ $completed != 1 ]]; then
        rm -f -- "$cache/.naki-object-keys.json" "$cache/install/share/naki/ffmpeg-build.json" \
            "$cache/vvenc/.naki-object-key" "$cache/vvenc/.naki-object-key.pending"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ -z $snapshot ]]; then
    snapshot=$(mktemp "$cache/vvenc/.source-inputs.XXXXXX.json")
    own_snapshot=1
    python3 "$root/scripts/sdk-identity.py" snapshot --snapshot "$snapshot"
fi
object_key=$(python3 "$root/scripts/sdk-identity.py" object-key --snapshot "$snapshot" --component vvenc)
if [[ $# -ge 2 ]]; then
    previous_key=$2
else
    previous_key=$(python3 "$root/scripts/sdk-identity.py" cache-key --build-root "$cache" --prefix "$cache/install" --component vvenc)
fi
python3 "$root/scripts/sdk-identity.py" invalidate-cache --build-root "$cache" --prefix "$cache/install"
# Keep VVenC's bundled JSON enabled: its one-pass lookahead RC uses that path
# for the internal first-pass cache too, even without a stats file.
cmake -S "$root/third_party/vvenc" -B "$cache/vvenc" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$cache/install" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DVVENC_LIBRARY_ONLY=ON \
    -DVVENC_TOPLEVEL_OUTPUT_DIRS=OFF -DVVENC_ENABLE_LINK_TIME_OPT=OFF \
    -DVVENC_ENABLE_THIRDPARTY_JSON=ON -DVVCEncoderLib_ENABLE_ITT=OFF \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -DNDEBUG -fno-omit-frame-pointer' \
    -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--build-id=sha1
clean=()
if [[ $previous_key != "$object_key" ]]; then
    clean=(--clean-first)
fi
cmake --build "$cache/vvenc" "${clean[@]}" -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
# A fresh build can have the same install timestamp as the old DSO/header.
# Force CMake's file(INSTALL) copy path rather than sealing stale installed bytes.
CMAKE_INSTALL_ALWAYS=1 cmake --install "$cache/vvenc"
# Archive before stripping; the runtime never needs DWARF. Keep exact Build IDs.
if [[ -n ${NAKI_VVENC_SYMBOLS:-} ]]; then
    python3 "$root/scripts/archive-symbols.py" --output "$NAKI_VVENC_SYMBOLS" \
        --label vvenc --source-revision "${VVENC_REVISION:-unknown}" \
        --dependency-key "${DEPENDENCY_KEY:-local}" "$cache/vvenc/lib"
    cp "$cache/vvenc/CMakeCache.txt" "$NAKI_VVENC_SYMBOLS/"
fi
find "$cache/install/lib" -maxdepth 1 -type f -name 'libvvenc.so*' -exec strip --strip-debug {} +
printf '%s\n' "$object_key" > "$cache/vvenc/.naki-object-key.pending"
# A standalone VVenC build remains uncommitted until the outer SDK seal.
completed=1
