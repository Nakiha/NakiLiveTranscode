#!/usr/bin/env bash
set -euo pipefail
if [[ -d /ucrt64/bin ]]; then export PATH="/ucrt64/bin:/usr/bin:$PATH"; fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
platform=${1:-linux}
case "$platform" in linux|windows) ;; *) echo 'Usage: test-decoder.sh [linux|windows]' >&2; exit 2;; esac
work="$root/experiments/decoder-20261016"
export FFMPEG_LIVE_INSTALL_DIR="$root/out/install/$platform-auto"
export FFMPEG_LIVE_FIXTURE_DIR="$work/results-msys2-baseline"
baseline=results-linux-baseline
[[ "$platform" == windows ]] && baseline=results-msys2-baseline
export FFMPEG_LIVE_BASELINE_RESULT_DIR="$work/$baseline"
for path in "$FFMPEG_LIVE_INSTALL_DIR" "$FFMPEG_LIVE_FIXTURE_DIR" "$FFMPEG_LIVE_BASELINE_RESULT_DIR"; do
    [[ -d "$path" ]] || { echo "Missing build or frozen test data: $path" >&2; exit 1; }
done
mkdir -p "$root/out/results"
FFMPEG_LIVE_RESULT_DIR=$(mktemp -d "$root/out/results/$platform-auto-XXXXXXXX")
export FFMPEG_LIVE_RESULT_DIR FFMPEG_LIVE_AUTO_MIN=2
echo "Results: $FFMPEG_LIVE_RESULT_DIR"
exec bash "$work/ffmpeg-7.0.3/tools/live-async/test-async.sh"
