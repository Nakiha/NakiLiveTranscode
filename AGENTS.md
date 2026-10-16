# Project conventions

- The application is not implemented yet. Do not describe decoder probes as a complete transcoder.
- FFmpeg lives in the pinned submodule `experiments/decoder-20261016/ffmpeg-7.0.3`. Preserve its independent Git history. Commit intentional changes there before updating the parent gitlink.
- Read the submodule's `doc/live-async-guide-zh.txt` and `doc/live-async-auto-zh.txt` before changing decoder integration.
- Historical baseline media, hashes, timings and reports are frozen evidence. Write new runs to unique directories under `out/`; never overwrite frozen results.
- Use `scripts/build-decoder.sh` and `scripts/test-decoder.sh` as the new decoder entry points. Archived root-level export/capture scripts contain obsolete paths.
- Keep generated libraries, objects, media, logs, virtual environments and offline bundles out of Git. Do not delete local archived experiments without a user request.
- Run appropriate checks for the actual change. Claims about latency or capacity require fresh measurement with its boundaries stated; documentation/organization changes do not justify rerunning the full benchmark matrix.
