#!/usr/bin/env bash
set -euo pipefail
mkdir -p out
# These tests generate media and exercise real sockets/processes; no archived test clips are needed.
python3 tests/sdk_identity_test.py
python3 tests/sdk_expected_test.py
python3 tests/release_manifest_test.py
python3 tests/sdk_build_integration.py
python3 tests/event_maintenance_test.py
out/bin/naki-frame-policy-test
out/bin/naki-frame-policy-config-test
out/bin/naki-watermark-config-test
out/bin/naki-branch-queue-test
out/bin/naki-identity-intent-test
out/bin/naki-control-spec-test
out/bin/naki-sei-test
out/bin/naki-component-update-test
out/bin/naki-operation-store-test
out/bin/naki-operation-cache-test
out/bin/naki-audio-signature-test
out/bin/naki-video-rate-test
event_logger_run=$(mktemp -d out/event-logger-XXXXXX)
out/bin/naki-event-logger-test "$event_logger_run"
out/bin/naki-video-filter-test
python3 tests/watermark_integration.py
python3 tests/watermark_review_integration.py
python3 tests/update_api_integration.py
python3 tests/multi_output_integration.py
python3 tests/mixed_queue_integration.py
python3 tests/audio_signature_integration.py
python3 tests/video_rate_integration.py
python3 tests/sei_integration.py
python3 tests/frame_policy_integration.py
python3 tests/probe_integration.py
python3 tests/decoder_integration.py
python3 tests/service_integration.py
python3 tests/vvenc_integration.py
python3 tests/vvenc_pts_integration.py
python3 tests/live_mux_integration.py
python3 tests/live_input_stability_integration.py
python3 tests/media_hooks_integration.py
python3 tests/vvenc_latency.py --repeats 1
python3 tests/observability_integration.py
python3 tests/process_integration.py
python3 tests/control_lifecycle_integration.py
python3 tests/health_progress_integration.py
python3 tests/output_finalize_integration.py
python3 tests/crash_integration.py
python3 tests/symbols_integration.py --ffmpeg-symbols /symbols/ffmpeg --vvenc-symbols /symbols/vvenc
