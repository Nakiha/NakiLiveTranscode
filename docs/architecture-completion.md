# Component boundary completion

This continues the first boundary-hardening commit. Completion is assessed by ownership and executable constraints, not file count or a promise that edge cases disappear.

- [x] Stable opaque mux, output I/O owners, control library independent of the media SDK.
- [x] Opaque worker Job outside worker implementation. Media reads immutable configuration/identity through a contract and publishes typed progress/counter/lifecycle events; no mutable Job/progress/statistics pointers or externally held Job mutex.
- [x] Job allocation, child/destination construction, remote projection and final classification remain worker-owned; report windows and first-frame boundaries retain their existing authority. Stage-specific media regression passed (see validation below).
- [ ] Input/decode state and filter/encode state have separate private representations and explicit close/drain APIs. Orchestration cannot access codec internals belonging to another component.
- [ ] Rendition holder, logical membership and operation phases use typed transitions. Removal, restart, EOF, late completion and exit proof have explicit, independently testable rules.
- [ ] Destination writer execution and attachment phases use typed transitions; readiness, applied and actual delivery stay separate.
- [ ] Build targets, public headers and dependency checks enforce these boundaries. Scope-specific tests and component instructions describe extension points.
- [ ] Final functional/sanitizer evidence binds the final source and dependencies; commits are independently reviewable and pushed to the work branch.

The fixed-output path and explicit process/thread rendition backends retain their existing public behavior. No codec, protocol, dynamic format, timeline repair or resource-capacity claim is added by this work. FFmpeg/VVenC source and frozen evidence are unchanged unless a separately justified dependency change becomes necessary.

## Job boundary validation

WSL build uses `scripts/build-service.sh`, a separate `architecture-completion` CMake directory and verified existing SDK. Evidence is under `out/architecture-completion-20261028/`. `job-contracts/summary.json` and `job-renditions/summary.json` both passed with source SHA256 `d250f2618a5a8bed19b6ca7341a7509a13e25d74de3c7ce50bc19f5cb135ef54`; source and binaries remained unchanged in each run. Root documentation updates after those runs do not enter the runner's application source snapshot.

The selected contract run covers SDK verification, all 30 native tests, organization, updates, multi-output, 86 lifecycle-media cases, audio signatures, service, observability and output finalization. The separate rendition gate covers process/thread operation, lifecycle/fault/recovery, destination delivery, audio/SEI, network reconnect, exit-proof lifetime, format/subscription races, churn, 32 matrix cases, two advanced and two sustained cases, and regression scenarios. The HE-AAC fixture uses the verified local copy documented in the first-round validation, avoiding a fixture download dependency. Quick checks also passed during development. This is focused stage evidence; final full functional and sanitizer coverage remains required after the remaining ownership changes.
