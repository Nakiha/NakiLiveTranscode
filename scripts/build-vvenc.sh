#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
# Keep VVenC's bundled JSON enabled: its one-pass lookahead RC uses that path
# for the internal first-pass cache too, even without a stats file.
cmake -S "$root/third_party/vvenc" -B "$cache/vvenc" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$cache/install" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DVVENC_LIBRARY_ONLY=ON \
    -DVVENC_TOPLEVEL_OUTPUT_DIRS=OFF -DVVENC_ENABLE_LINK_TIME_OPT=OFF \
    -DVVENC_ENABLE_THIRDPARTY_JSON=ON -DVVCEncoderLib_ENABLE_ITT=OFF \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -DNDEBUG -fno-omit-frame-pointer' \
    -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--build-id=sha1
cmake --build "$cache/vvenc" -j"${FFMPEG_LIVE_BUILD_JOBS:-8}"
cmake --install "$cache/vvenc"
# Archive before stripping; the runtime never needs DWARF. Keep exact Build IDs.
if [[ -n ${NAKI_VVENC_SYMBOLS:-} ]]; then
    python3 "$root/scripts/archive-symbols.py" --output "$NAKI_VVENC_SYMBOLS" \
        --label vvenc --source-revision "${VVENC_REVISION:-unknown}" \
        --dependency-key "${DEPENDENCY_KEY:-local}" "$cache/vvenc/lib"
    cp "$cache/vvenc/CMakeCache.txt" "$NAKI_VVENC_SYMBOLS/"
fi
find "$cache/install/lib" -maxdepth 1 -type f -name 'libvvenc.so*' -exec strip --strip-debug {} +
