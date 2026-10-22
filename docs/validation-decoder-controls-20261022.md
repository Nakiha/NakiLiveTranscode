# Decoder controls: validation window, 2026-10-22

**实验版，最终验收未完成，不建议生产启用**

This first batch fixes the validation foundation and adds independently selectable
service backends. It does not optimize H264's phase3 setup deferral and does not
claim better production latency or capacity. Historical archives and original
uncommitted workspaces are preserved.

## Completed before the initial controls commit

- Active FFmpeg validation commit:
  `2673319b815fa6da6fccd0c3801e099b2e473066`. Codec source is unchanged from
  phase3; only active probes, checks and a state-separation design boundary were
  added. This commit is the explicit experimental SDK pin in this branch.
- 18 judge cases passed, including zero output, missing complete cycles, failed
  process/terminal, timeout, missing completion markers, duplicate frame events,
  source mismatch, narrow DTS exceptions and separate close semantics.
- A fresh 83-case macOS arm64 matrix against frozen baseline/phase3 SDKs passed:
  81 ordered-frame comparisons plus two in-flight-close lifetime checks. Counts
  are declared independently, with explicit cycle completion; output files are
  unique. Three certificate cases were repeated after adding the explicit
  `certificate_verified=1` gate. Deliberately wrong binary identity returns
  runner exit 1. Reusing an existing result directory is rejected without changing
  its plan hash. See the FFmpeg active tools' `VALIDATION.json` for scope/hash.
- The new application configuration test compiled and ran on macOS against the
  frozen phase3 SDK and pinned yyjson. It covers default/fingerprint compatibility,
  strict boolean types, backend/pump conflicts, all four independent request
  identities and branch configuration inheritance. The macOS-only link used the
  existing identifier-validation adapter; Linux CTest links the real controller
  library instead.
- The service's actual backend helpers ran against five existing 192-frame
  inputs: HRD H264 B3, B7, dynamic B, HEVC B7 and no-HRD H264 B7. For each input,
  sync/async-fixed/async-auto x early off/on completed three full replay cycles
  with ordered pixel/PTS/source-ID/recorded metadata agreement, except allowed
  early-on trigger pkt_dts. This is a backend harness, not the full service.
- Six organization checks passed on macOS using a canonical temporary directory.
  The general entrypoint suite is Linux-specific; its macOS run retained the
  expected platform-gate mismatch rather than being presented as a full pass.

No fixture media was generated for these checks. Existing inputs and frozen
logs/builds were not overwritten. Fresh results, native binaries and raw logs
remain local under a new phase4 directory and are not committed.

## Linux development environment and incomplete gates at this commit

Docker CLI/Compose and a separate Colima development VM were installed without
requiring desktop interaction. The Docker daemon successfully pulled and ran
ARM64 hello-world. A native Ubuntu ARM64 SDK attempt reached the existing VVenC
fork's x86 Native8 intrinsic include (`immintrin.h`) and failed; it is not a
successful service build. The service SDK identity gate explicitly targets
Linux x86_64, so an amd64 Ubuntu development container was started successfully
under emulation for the supported build path. Dependency installation and the
complete-service build/test are pending at this initial commit.

Emulated container execution can validate build/functionality but must not be
used as production CPU/throughput/latency evidence. The new full-service
`decoder-controls` suite is registered but is **not yet claimed passed here**.
It will require explicit complete frame counts, source/PTS ordering, correct
backend status and decoded output count in all four combinations.

## Remaining acceptance

Restore H264 frame parallelism through a reviewed separation of decode and
presentation state, retain whole-AU fail-closed and atomic output preparation,
and then run the malformed-AU/OOM/flush/thread gates on the new algorithm.
Expand Linux service adversarial/lifecycle checks and complete real-platform
sustained-load validation separately:
3B/7B/dynamic B, CPU per frame, unrestricted throughput, queue age/growth,
first-frame startup and ingress-to-actual-mux P95/P99. No new performance result,
unrestricted H264 fast-output coverage or final acceptance follows from this batch.

## Follow-up: Linux functional integration completed

The initial pending status above records the initial controls commit. Subsequent
development used Ubuntu 24.04 amd64 in the dedicated Colima VM on this macOS
ARM host. The first QEMU GCC build failed with an internal compiler segmentation
fault in existing `libavformat/vvc.c`; a single-file recheck passed. The VM was
then switched to already-installed Rosetta, and the **complete unmodified SDK
recipe** and `scripts/build-service.sh` both succeeded. No identity gate was
bypassed. The experimental FFmpeg pin is still
`2673319b815fa6da6fccd0c3801e099b2e473066`.

- Sealed SDK identity:
  `abc4c05d75393cf97edc5996c6ff1ca48a67eb918e60512ba6ed3dfdd120a334`.
- Linux native quick CTest: 20/20 passed, including the new controls test.
- Linux entrypoint checks: 14/14 passed; organization checks: 6/6 passed.
- Five existing inputs, each independently declared as 192 frames, passed all
  four real controller/worker configurations. Each used `max_threads=2`,
  `min_threads=2`, `decoder_auto_threads=false`, `decoder_pump=false`,
  `queue_capacity=1`, unpaced file input and one encoder thread.

| Existing input | Sync / early off | Sync / early on | Async / early off | Async / early on |
| --- | --- | --- | --- | --- |
| HRD H264 fixed B3, pyramid on, closed GOP | PASS | PASS | PASS | PASS |
| HRD H264 fixed B7, pyramid on, closed GOP | PASS | PASS | PASS | PASS |
| HRD H264 adaptive B16, pyramid on, closed GOP | PASS | PASS | PASS | PASS |
| HEVC fixed B7, pyramid on, closed GOP | PASS | PASS | PASS | PASS |
| No-HRD H264 fixed B7, pyramid on, closed GOP | PASS | PASS | PASS | PASS |

PASS requires successful service completion, exactly 192 decoded/filtered/output
packets and sealed frame traces, the complete input-ID set, ordered and matching
cross-backend PTS/source/picture metadata, correct backend/request status and
finite nonnegative new timing fields. Each actual encoded output was independently
decoded/count-checked by ffprobe as 192 frames. The backend probe additionally
passed sync/async-fixed/async-auto x early off/on, three replay cycles per mode,
on each of the five inputs: 90 cycles and 17,280 decoded frames, including ordered
pixels/PTS/source identities and recorded metadata with only the documented
early-on pkt_dts exception.

The service runner now writes its declared frame count, four-case plan, input
hash and executable hashes before running. Deliberately declaring 191 frames
for the 192-frame input returned exit 1 with no COMPLETE marker. Reusing a result
directory also returned exit 1 and preserved the prior plan hash. Neither check
was counted as a successful media case.

Service executable SHA256 values:

```text
naki-live                  e65de4cf6f5721a2f1501e94e482ec549c2fb392b08d9607b512fd067c708959
naki-live-worker           6d5647ae020b3f12990ecf5f84e3b0746e13130077d5fcceeeefaa65113a335d
naki-decoder-backend-probe 59709ecb22e64236d4d20fd90e3673acc5abe58cf65bacb8f86b232eb18bf7c7
```

The locally retained aggregate service evidence has SHA256
`41bcd4ab1aa118a7e0c5da96755a82259b16e23c6e1d2142b99303ed0b60100f`.
Raw logs, media, SDKs and binaries remain outside Git. Original workspaces were
checked against the preservation snapshot: HEAD, status, index and saved changed
file hashes still match.

These short emulated runs establish this functional scope only. They provide
**no accepted CPU, capacity, queue-stability, startup or P95/P99 comparison**.
They do not prove that every early-on request hit the fast path; status reports
the request, not per-frame eligibility. H264 setup deferral, HRD/CFR/single-slice
restrictions and no-declaration fallback remain unchanged. The redesign and
final independent review are still pending.
