# Component boundary completion

This continues the first boundary-hardening commit. Completion is assessed by ownership and executable constraints, not file count or a promise that edge cases disappear.

- [x] Stable opaque mux, output I/O owners, control library independent of the media SDK.
- [x] Opaque worker Job outside worker implementation. Media reads immutable configuration/identity through a contract and publishes typed progress/counter/lifecycle events; no mutable Job/progress/statistics pointers or externally held Job mutex.
- [x] Job allocation, child/destination construction, remote projection and final classification remain worker-owned; report windows and first-frame boundaries retain their existing authority. Stage-specific media regression passed (see validation below).
- [x] Input/decode state and filter/encode state have separate private representations and explicit close/drain APIs. Fixed branch state is opaque too. Orchestration cannot access codec internals belonging to another component; destination writers retain deep sealed parameters and scalar stream timing.
- [x] Rendition holder, logical membership and operation phases use typed transitions. Removal, restart, EOF, late completion and exit proof have explicit, independently testable rules.
- [x] Destination writer execution and attachment phases use typed transitions; readiness, applied and actual delivery stay separate.
- [x] Build targets, public headers and dependency checks enforce these boundaries. Scope-specific tests and component instructions describe extension points.
- [x] Final functional/sanitizer evidence binds the final application/build sources and dependencies, with the sole later service-test readiness fix explicitly tracked; commits are independently reviewable and pushed to the work branch.

The fixed-output path and explicit process/thread rendition backends retain their existing public behavior. No codec, protocol, dynamic format, timeline repair or resource-capacity claim is added by this work. FFmpeg/VVenC source and frozen evidence are unchanged unless a separately justified dependency change becomes necessary.

## Job boundary validation

WSL build uses `scripts/build-service.sh`, a separate `architecture-completion` CMake directory and verified existing SDK. Evidence is under `out/architecture-completion-20261028/`. `job-contracts/summary.json` and `job-renditions/summary.json` both passed with source SHA256 `d250f2618a5a8bed19b6ca7341a7509a13e25d74de3c7ce50bc19f5cb135ef54`; source and binaries remained unchanged in each run. Root documentation updates after those runs do not enter the runner's application source snapshot.

The selected contract run covers SDK verification, all 30 native tests, organization, updates, multi-output, 86 lifecycle-media cases, audio signatures, service, observability and output finalization. The separate rendition gate covers process/thread operation, lifecycle/fault/recovery, destination delivery, audio/SEI, network reconnect, exit-proof lifetime, format/subscription races, churn, 32 matrix cases, two advanced and two sustained cases, and regression scenarios. The HE-AAC fixture uses the verified local copy documented in the first-round validation, avoiding a fixture download dependency. Quick checks also passed during development. This is focused stage evidence; final full functional and sanitizer coverage remains required after the remaining ownership changes.

## Native owner separation validation

`owner-contracts-final/summary.json` passed all eight selected steps with source SHA256 `5ff496b93c64bf366b694cdabf6bf9b6db554d78389b0a637d3d96d21939595f`: SDK verification, 30 native quick contracts, the complete rendition gate, organization (including compile-negative opaque-state checks), codec phases, fixed multi-output, all 86 lifecycle-media cases and output finalization. Source, binaries and SDK manifest were unchanged through the run. Separate quick and all-native runs (also including the two native integration helpers) passed during development; the final owner build had no warnings. This stage does not constitute final coverage of the following lifecycle changes.

## Completion evidence

All items above are implemented. Final normal full passed 44/44 and all-native passed 33/33. Sanitizer coverage is completed in two windows after correcting a service test's acceptance/readiness race; application/build sources and corresponding binaries are identical between windows. Exact source digests, successful runs, the retained failure and the scope of the sole test-only change are documented in [completion validation](validation-architecture-completion-20261029.md). This does not remove runtime fault cases or authorize skipping the relevant integration checks.
