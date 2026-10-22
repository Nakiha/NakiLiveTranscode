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
Complete Linux service and real-platform sustained-load validation separately:
3B/7B/dynamic B, CPU per frame, unrestricted throughput, queue age/growth,
first-frame startup and ingress-to-actual-mux P95/P99. No new performance result,
unrestricted H264 fast-output coverage or final acceptance follows from this batch.
