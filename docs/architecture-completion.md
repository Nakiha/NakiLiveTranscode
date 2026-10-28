# Component boundary completion

This continues the first boundary-hardening commit. Completion is assessed by ownership and executable constraints, not file count or a promise that edge cases disappear.

- [x] Stable opaque mux, output I/O owners, control library independent of the media SDK.
- [x] Opaque worker Job outside worker implementation. Media reads immutable configuration/identity through a contract and publishes typed progress/counter/lifecycle events; no mutable Job/progress/statistics pointers or externally held Job mutex.
- [x] Job allocation, child/destination construction, remote projection and final classification remain worker-owned; report windows and first-frame boundaries retain their existing authority. Stage-specific media regression passed (see validation below).
- [x] Input/decode state and filter/encode state have separate private representations and explicit close/drain APIs. Fixed branch state is opaque too. Orchestration cannot access codec internals belonging to another component; destination writers retain deep sealed parameters and scalar stream timing.
- [ ] Rendition holder, logical membership and operation phases use typed transitions. Removal, restart, EOF, late completion and exit proof have explicit, independently testable rules.
- [ ] Destination writer execution and attachment phases use typed transitions; readiness, applied and actual delivery stay separate.
- [ ] Build targets, public headers and dependency checks enforce these boundaries. Scope-specific tests and component instructions describe extension points.
- [ ] Final functional/sanitizer evidence binds the final source and dependencies; commits are independently reviewable and pushed to the work branch.

The fixed-output path and explicit process/thread rendition backends retain their existing public behavior. No codec, protocol, dynamic format, timeline repair or resource-capacity claim is added by this work. FFmpeg/VVenC source and frozen evidence are unchanged unless a separately justified dependency change becomes necessary.

## Job boundary validation

WSL build uses `scripts/build-service.sh`, a separate `architecture-completion` CMake directory and verified existing SDK. Evidence is under `out/architecture-completion-20261028/`. `job-contracts/summary.json` and `job-renditions/summary.json` both passed with source SHA256 `d250f2618a5a8bed19b6ca7341a7509a13e25d74de3c7ce50bc19f5cb135ef54`; source and binaries remained unchanged in each run. Root documentation updates after those runs do not enter the runner's application source snapshot.

The selected contract run covers SDK verification, all 30 native tests, organization, updates, multi-output, 86 lifecycle-media cases, audio signatures, service, observability and output finalization. The separate rendition gate covers process/thread operation, lifecycle/fault/recovery, destination delivery, audio/SEI, network reconnect, exit-proof lifetime, format/subscription races, churn, 32 matrix cases, two advanced and two sustained cases, and regression scenarios. The HE-AAC fixture uses the verified local copy documented in the first-round validation, avoiding a fixture download dependency. Quick checks also passed during development. This is focused stage evidence; final full functional and sanitizer coverage remains required after the remaining ownership changes.

## Native owner separation validation

`owner-contracts-final/summary.json` passed all eight selected steps with source SHA256 `5ff496b93c64bf366b694cdabf6bf9b6db554d78389b0a637d3d96d21939595f`: SDK verification, native tests (30 quick contracts plus two native integration helpers), the complete rendition gate, organization (including compile-negative opaque-state checks), codec phases, fixed multi-output, all 86 lifecycle-media cases and output finalization. Source, binaries and SDK manifest were unchanged through the run. Separate quick and native runs passed during development; the final owner build had no warnings. This stage does not constitute final coverage of the following lifecycle changes.
