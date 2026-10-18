# Experimental candidate acceptance and migration

This is an experimental Linux/x86_64 service. Windows is the development host; builds and media workers run in Linux/WSL. A local matrix pass does not establish production readiness or target-host capacity. Native FFmpeg is n9.0.2 at the pinned historical submodule path; frozen n7.0.3 measurements remain historical evidence.

The five local windows are indexed in [validation-core-refactor.md](validation-core-refactor.md); the latest source review and candidate validation are in [validation-final-review.md](validation-final-review.md). Local evidence does not establish an actual image build/smoke, remote Actions, collector ingestion or target-host migration/rollback. Check the corresponding PR/commit's CI and deployment records separately. This runbook is a procedure, not evidence those gates passed.

## Reviewable candidate

1. Keep the previous image, SDK receipt, source manifest and all application/FFmpeg/VVenC Build ID debug files. Do not overwrite archived experiments or prior evidence directories.
2. Capture `scripts/release-manifest.py source` before building. Build through `scripts/build-service.sh`, using the SDK consumer expectation gate and at most the configured compiler jobs. Archive separate DWARF before stripping copies of runtime executables.
3. Record the exact source digest, commit, dirty state, SDK content identity/key/pins, actual runtime ELF/debug Build IDs and byte hashes. For an image candidate also retain the actual image inspection, unique commit/run/attempt tag, image ID and archive hashes. Verify those bytes with `release-manifest.py verify`.
4. Use a unique `out/` directory for every candidate. Record each test command, exit status and evidence path. Compare source-input manifests before and after testing; changed source invalidates the combined candidate claim. Preserve failures as well as successful retries.
5. A local dirty/no-image manifest remains `publication_eligible=false`. Actual image build, extraction, smoke test and manifest verification are CI gates. Do not promote a manifest merely because it is internally parseable or its source commit is familiar.

The maintained functional entry point is `bash scripts/test-service.sh full --ffmpeg-symbols <directory> --vvenc-symbols <directory>`; use `scripts/test-sanitizers.sh` for the separate application ASan/UBSan gate. See [testing.md](testing.md) for the quick/full inventory, prerequisites, identity records and explicit external gates. The earlier `out/engineering-plan-20261018/run-candidate.sh` and `candidate-current.txt` belong to the historical local engineering workflow, not the maintained test entry point. Its frozen results remain authoritative only with `complete.exit=0`, unchanged source inputs and the individual logs. Documentation does not declare an in-progress run passed.

## Acceptance boundaries

| Gate | Required evidence |
| --- | --- |
| SDK/source identity | Trusted checkout expectation, transitive VVenC dependency key, stale-cache and failure/recovery tests; actual installed bytes |
| Ownership/control | Worker ACK before acceptance; original creation/update/stop retries; EAGAIN/incompatible recovery keeps admission closed while media survives; verified peer/pidfd stop; launcher drain |
| Output lifecycle | Single and multiple outputs, trailer/flush/close failure, retained original primary error, no healthy sibling cancellation, bounded shutdown |
| Updates | Sparse desired merge/CAS/dedup/capacity, actual applied and first-written frame identities, B-picture reorder, not-written only after closure, worker-loss unknown outcome, event-I/O loss, late-result version ordering |
| Media bounds | Default and smooth TS, video plus 16 AAC under 2/4 owners, independent type/byte/work/age limits and pressure hysteresis, explicit drops/rejections, all retained references closed |
| Audio/rate | Actual same-PID AAC/MP3 changes, actual ASC sequence headers, stable official HE-AAC, rational rate/fallback rules, source VFR/offset/gaps, ordinary-I versus output-key, VVC tails/reorder |
| Health/progress | Busy remains ready, admission/recovery separate, real paused worker/controller, held encoded frame, live waiting input remains manageable, scoped progress and stale snapshot evidence |
| Crash/symbols | Bounded external collector, exact actual ELF/DWARF, real PIE source-line resolution, wrong/missing/corrupt symbols rejected, no global core-pattern changes |
| Collector/host | Actual collector receives the process event schema; agreed host budgets and load; 24/72-hour soak and migration/rollback evidence |

The final row requires the real environment. Configuration examples, local logger faults and short regression runs do not substitute for it. Unknown-rate rate-selection evidence currently includes a deliberately injected unknown library result; the real encoded source and PTS remain intact, and that test does not claim a source file without rate metadata.

## Target host preparation

Set explicit CPU, memory, PID, FD and standby-worker budgets from measurement. `deploy/budget.example.env` is illustrative, not a supported capacity profile. Compose preserves unbounded CPU/memory when those knobs are unset; the existing PID default and explicit FD limits are separate. Confirm actual cgroup settings and kernel peer/pidfd support, SDK architecture and native library IDs before admitting traffic. Readiness requires reliable control ownership, not an idle task.

Run the actual log collector using [process-events.md](process-events.md) and the Filebeat example. Confirm event identity/sequence, job/output/operation/revision correlation, typed phase fields, rotation, sealed-only retention, dropped-status and disk-error reporting. Never treat event ingestion as a durable operation journal. Do not externally compress an active file.

For 24/72-hour runs, retain source/media hashes, input protocols, codec/rate/layout, resolution, output count/encoders, audio timing, network/load shape, cgroup budget and sampling intervals. Record FD/socket/PID high-water, retained queue/reader/encoder/mux data, process RSS and cgroup memory separately, CPU use, logger volume/disk and restarts. Report completed-frame latency and in-flight age together. The service ingress-to-mux boundary includes queue residence and actual write/flush; it excludes source capture, client upload/network and peer display. Do not turn socket timings into wire/frame correlation.

No automatic capacity or latency threshold is introduced without agreed measurements/SLO. Existing hard byte/count/profile limits and I/O/shutdown deadlines still apply. If load is outside the measured budget, reject or reduce the deployment profile before traffic; do not use an OOM kill as an admission strategy.

## Controlled migration and rollback

1. Verify the old/new manifests and both debug archives offline. Keep old state/event directories recoverable. Controller instance IDs change on restart; the configured service namespace and private state location must remain stable.
2. Close new-task admission at the routing/deployment layer. Observe current task, operation outcomes, output progress and snapshot freshness. A busy task should remain queryable/stoppable/updateable through its existing controller.
3. Use the explicit job target and a stable stop operation ID if draining the task. Await worker confirmation/terminal state and verify zero retained queue/frame references. Service SIGTERM intentionally stops workers; a controller crash/restart instead recovers verified existing workers before opening admission.
4. Start the verified candidate under the agreed budget. Probe liveness, then readiness, then admission independently. Check actual loaded native IDs, event ingestion and first-frame checkpoints with the correct boundary. Do not admit a second task while recovery is unresolved.
5. On a failed gate, close new admission and retain the failure evidence. Drain through verified ownership, restore the previous verified image/config, recover or explicitly stop existing workers as appropriate, and recheck the same gates. Do not signal a numeric PID whose identity has not been verified.

Code, local tests and this runbook can be delivered without committing, pushing, creating a release or deploying. Actual migration, rollback on the target host and external acceptance remain separately authorized actions.
