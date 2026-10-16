# Project conventions

- The application is an experimental Linux-only, video-only single-stream C service. Do not describe it as production-ready or claim audio/dynamic-format support.
- Build the service with `scripts/build-service.sh` in Linux/WSL. JSON is yyjson, HTTP server is libevent evhttp, and the CLI client uses libcurl. Preserve this separation from FFmpeg media protocols.
- Trace latency using reference-counted opaque_ref through decode/filter and a bounded PTS ledger through encode. Missing mappings must fail explicitly. Never call service ingress-to-mux measurements source-to-screen latency.
- Latency metrics sample every completed frame; only trace detail is sampled. Preserve separate input I/first I/recurring I and output-key classifications, histogram bounds, and nonoverlapping reporting windows.
- The logger owns all logging I/O and rotation. Producers must never wait for log I/O; bounded queue overflow is counted, including status loss. Only closed sealed trace segments are eligible for external compression/retention.
- Controller loss must not cancel media workers. Recover existing private worker endpoints before admitting a new task; use peer credentials and pidfds rather than signalling an unverified PID. SIGTERM of the service intentionally stops all workers.
- IPC carries bounded control/status/log JSON, never AVFrame/AVPacket pointers or raw frames. Preserve unconditional first-frame startup checkpoints and their explicit HTTP-handler-to-mux boundary; client network/upload and display time are outside that measurement.
- Fatal handlers may only send the fixed crash record and use bounded signal-safe/Linux terminal operations. No yyjson/logger/allocation/application locks/unwinding there. The independent collector owns crash-report I/O, caps capture/JSON/retention, and must not leave a stopped target waiting indefinitely. Default fatal exit suppresses full kernel core without changing global core_pattern.
- TCP/UDP observation is a fork-only lifecycle API in libavformat/network_observer.h. Its callbacks may run on network background threads: no allocation, blocking, logging or FFmpeg reentry. Socket timing is not wire time or frame correlation.
- FFmpeg lives in the pinned submodule `experiments/decoder-20261016/ffmpeg-7.0.3`. Preserve its independent Git history. Commit intentional changes there before updating the parent gitlink.
- Read the submodule's `doc/live-async-guide-zh.txt` and `doc/live-async-auto-zh.txt` before changing decoder integration.
- Historical baseline media, hashes, timings and reports are frozen evidence. Write new runs to unique directories under `out/`; never overwrite frozen results.
- Use `scripts/build-decoder.sh` and `scripts/test-decoder.sh` as the new decoder entry points. Archived root-level export/capture scripts contain obsolete paths.
- Keep generated libraries, objects, media, logs, virtual environments and offline bundles out of Git. Do not delete local archived experiments without a user request.
- Run appropriate checks for the actual change. Claims about latency or capacity require fresh measurement with its boundaries stated; documentation/organization changes do not justify rerunning the full benchmark matrix.
