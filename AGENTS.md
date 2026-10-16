# Project conventions

- The application is an experimental Linux-only, video-only single-stream C service. Do not describe it as production-ready or claim audio/dynamic-format support.
- Build the service with `scripts/build-service.sh` in Linux/WSL. JSON is yyjson, HTTP server is libevent evhttp, and the CLI client uses libcurl. Preserve this separation from FFmpeg media protocols.
- Trace latency using reference-counted opaque_ref through decode/filter and a bounded PTS ledger through encode. Missing mappings must fail explicitly. Never call service ingress-to-mux measurements source-to-screen latency.
- FFmpeg lives in the pinned submodule `experiments/decoder-20261016/ffmpeg-7.0.3`. Preserve its independent Git history. Commit intentional changes there before updating the parent gitlink.
- Read the submodule's `doc/live-async-guide-zh.txt` and `doc/live-async-auto-zh.txt` before changing decoder integration.
- Historical baseline media, hashes, timings and reports are frozen evidence. Write new runs to unique directories under `out/`; never overwrite frozen results.
- Use `scripts/build-decoder.sh` and `scripts/test-decoder.sh` as the new decoder entry points. Archived root-level export/capture scripts contain obsolete paths.
- Keep generated libraries, objects, media, logs, virtual environments and offline bundles out of Git. Do not delete local archived experiments without a user request.
- Run appropriate checks for the actual change. Claims about latency or capacity require fresh measurement with its boundaries stated; documentation/organization changes do not justify rerunning the full benchmark matrix.
