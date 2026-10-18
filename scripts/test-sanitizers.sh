#!/usr/bin/env bash
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Linux / WSL is required.' >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache=${NAKI_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/naki-live-transcode-n9}
mkdir -p "$root/out"
run=$(mktemp -d "$root/out/sanitizers-XXXXXX")
printf 'Sanitizer build evidence: %s\n' "$run"
# Reuse the verified, uninstrumented native SDK; keep normal binaries separate.
export NAKI_SERVICE_BUILD_DIR="$cache/service-sanitizers"
export NAKI_SERVICE_BIN="$run/bin"
export NAKI_SANITIZERS=ON
export NAKI_BUILD_TYPE=Debug
# Required checks cannot be disabled by inherited sanitizer options. Deliberate
# fatal signal and preload/symbol tests stay in the normal full suite.
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export FFMPEG_LIVE_BUILD_JOBS=${FFMPEG_LIVE_BUILD_JOBS:-2}
if ! NAKI_SKIP_FFMPEG=1 bash "$root/scripts/build-service.sh" > "$run/build.log" 2>&1; then
    tail -60 "$run/build.log" >&2
    exit 1
fi
exec bash "$root/scripts/test-service.sh" sanitize "$@"
