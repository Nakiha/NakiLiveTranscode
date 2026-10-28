# Worker ownership

- `job.h` is private to enumerated worker implementation files. Never expose Job representation, mutex or writable statistics/progress pointers to media/controller contracts.
- `job_media.h` borrows immutable config/identity and existing lifetime-bound handles, and accepts typed observations. The worker owns every completed frame's latency sampling, nonoverlapping windows, input-I/first-I/recurring-I/output-key groups, first-frame checkpoint authority and terminal classification. Trace-detail sampling must not reduce metric sampling.
- `job_remote.h` owns remote projection and child/destination/slot factories. Keep partial-allocation rollback and allocator-role checks. Adapters prove actual holder exit/join before retirement; factories must not infer exit from a status message or unverified PID.
- Update `scripts/check-architecture.py` only for an intentional reviewed authority change. `job-media`, identity/update contracts and the relevant media lifecycle regression cover this boundary. Do not add FFmpeg dependencies to `naki-control` or send raw frames/native pointers in control JSON.
