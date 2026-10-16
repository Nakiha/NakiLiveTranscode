#!/usr/bin/env bash
set -euo pipefail
mkdir -p out
# These tests generate media and exercise real sockets/processes; no archived test clips are needed.
python3 tests/decoder_integration.py
python3 tests/service_integration.py
python3 tests/vvenc_integration.py
python3 tests/vvenc_pts_integration.py
python3 tests/vvenc_latency.py --repeats 1
python3 tests/observability_integration.py
python3 tests/process_integration.py
python3 tests/crash_integration.py
python3 tests/symbols_integration.py --ffmpeg-symbols /symbols/ffmpeg --vvenc-symbols /symbols/vvenc
