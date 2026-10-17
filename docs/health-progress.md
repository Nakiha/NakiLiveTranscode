# Control health and media progress

| Query | Meaning |
| --- | --- |
| `GET /health/live` | The controller HTTP event loop can answer; not proof of media output |
| `GET /health/ready` | Existing control ownership is reconciled and an active worker's control channel is available/fresh; a healthy busy task remains ready |
| `GET /health/admission` | A new creation can be attempted under the single-active-job, recovery, retained-creation and worker/backoff bounds |
| `GET /health` | Compatible capability query, always HTTP 200 while the handler is available; also includes all three structured decisions |

The dedicated ready/admission queries return HTTP 503 when closed, with an explicit reason. Busy closes admission, not readiness. Recovery, unknown/incompatible ownership and active worker control staleness close readiness. Readiness observes a two-second control-message age; it does not add a kill policy. A response cannot prove future acceptance or an unmeasured host capacity. Matching retained operation retries retain their existing replay semantics.

Root/output `progress` reports the last input admission and last video decoded/encoded/successfully written monotonic times and ages, sampled under the job mutex. Root input means reader-ring admission; branch input means admission by that output owner. A branch's decoded time is the shared decoder's output time and can precede branch admission. Encoded means actual packet receive before extension hooks; written means actual local mux write/AVIO completion. None means that stage has not been observed. These are service boundaries, not source capture, wire or display times.

`phase` distinguishes starting, waiting assets, opening input, waiting input, running, draining and terminal states. Waiting input is an owner currently needing further input; it can coexist with delayed encoder/decoder references. It does not imply failure or empty codec internals. The existing `write_phase` separately identifies a mux/flush/finalize call in progress.

Bounded video tracking now observes the existing packet/frame lifetime ledger even when overload dropping is disabled. The tracker retains at most 512 metadata records per job; overflow fails explicitly. Metadata removes on decoded input release, actual written output, dropped selection or teardown. Packet destructors close references for packets that never decode. No overload thresholds or new automatic drops are enabled by this observation. `oldest_video_inflight` includes the packet/frame ledger and a branch's retained video frames; mixed FIFO age is separate. The counters name these scopes explicitly and do not claim to describe all audio codec internals or RSS.

`freshness` distinguishes the worker's snapshot generation and generation time, controller receipt time and current controller observation time. `receipt_age_ms` and `stale` are controller observations; worker-generated progress ages belong to that snapshot's `sampled_monotonic_us`. A frozen worker snapshot must not be displayed as a new media observation merely because the controller still answers HTTP. Final retained snapshots can age while the task is inactive. A stopped controller cannot answer liveness, while independent workers can keep processing.

`/health` also exposes a bounded controller FD count (capped at 4096 entries and including observation overhead), NOFILE limits, and readable mounted cgroup-v2 root values for memory, pids and CPU quota. The cgroup scope is stated; it is not process RSS or necessarily an individual worker's allocation. Missing values are null. These observations do not introduce a resource-based kill or admission threshold. The existing media byte/count/thread/profile bounds remain the actual application gates.

Use explicit target-host CPU, memory, PID and FD budgets before deployment; the compose environment knobs allow that without claiming throughput for the example values. Measure RSS, per-worker/collector/thread counts, socket/FD high-water, retained queue bytes and log disk/traffic under the actual load. Application retained-byte limits are separate from cgroup RSS limits. Actual target budgets, collector ingestion and 24/72-hour soak remain external acceptance requirements until recorded on that host.

`tests/health_progress_integration.py` covers a busy ready task, a real SIGSTOP worker and stale control snapshot, a real SIGSTOP controller while its worker continues, a held actual encoded frame before first write, and an open HTTP media source waiting for more input with two output owners. The lifecycle suite separately checks live recovery EAGAIN/incompatible ownership and continued media. Pass/fail evidence is recorded per candidate, not implied by this interface document.
