# Application component boundaries

This describes the implemented maintenance boundaries, not additional media capabilities. The service remains experimental Linux/WSL software. Existing fixed-output and explicit rendition backend contracts remain in force.

## Enforced dependencies

`scripts/check-architecture.py` runs without a native SDK, is an `ALL` build target required by `naki-control`, and runs inside the quick/full organization suite. It checks local dependencies (including angle and relative includes), enumerated private-header consumers, transitive leakage of mutable state, implementation-file includes, and the owners of output protocol/mux APIs. Tests inject illegal dependencies and packet writers to verify rejection.

| Component | Contract |
| --- | --- |
| control | Plain C/yyjson policy, configuration, identity and operation state; no media SDK includes or link dependency. SHA256 uses OpenSSL Crypto; canonical intent bytes and fingerprints remain unchanged. |
| controller | Control/status projections; never includes mutable worker or pipeline state, including through wrappers. Its existing SDK capability reporting remains an explicit dependency. |
| worker | Task lifecycle, control loop and metrics publication; media calls go through the media interfaces. |
| media orchestration | Holds opaque component handles and timing descriptions; schedules input, output and branches through explicit owner APIs. |
| media input | `naki-media-input` owns demux, reader ring, decoder/async transfer, dispatch pump, source lineage/timeline and their synchronization. |
| media video | `naki-media-video` owns filter generations, compositor, encoder, PTS ledger and rate feedback. |
| media output | `naki-media-output` owns output setup/finalization, packet write/flush, audio registration and audio clock state. |
| media transport leaves | Shared pool, frame/audio wire codecs, messages and idle policy do not consume mutable Job/Pipeline state. |

The checker protects normal maintenance mistakes. A contributor who changes the checker can change the policy; code review remains necessary for deliberate ownership changes. Source checks do not prove race freedom or replace runtime lifetime tests.

## Input, video and immutable output descriptions

`NakiInput` is private to input initialization, reader and decode implementation. `NakiVideo` is private to video/filter and encode implementation. Fixed branch slots and their queues/threads are private to `pipeline_branches.c`. A Pipeline consumer cannot inspect any of these representations: compile-negative checks exercise decoder, encoder, mux, branch and encoded-description access. Native input open/read, encoder send/receive and parameter sealing APIs have enumerated source owners as well as private-header constraints.

Input captures only scalar initial frame properties before transferring the decoder to async ownership. Orchestration polls/feeds through input APIs, retaining the existing pending-packet/EAGAIN behavior. Cleanup wakes and joins the pump/reader, observes final decoder statistics, then frees async before destroying notification FDs. After mux finalization, the explicit release sequence retains receive-frame, synchronous-decoder, encoder/filter/compositor, demux and ledger retirement order. An output-only Pipeline owns no input state; empty and partial-initialization cleanup accepts absent components.

Once codec open succeeds, `NakiEncodedDescription` seals a deep copy of codec parameters and timing. Every destination holds its own reference until its writer joins. The mux copies those parameters and publishes scalar stream timing after header negotiation; no `AVStream *` or native encoder context crosses to a destination writer. Primary mux transfer retains scalar timing on the encoding side and no longer allocates a fake format context/stream. Mux-owned key-grid history travels with the same mux allocation. IDR requests and actual-write rate feedback return through video-owner APIs; their calling thread and existing capability restrictions remain in force.

## Stable mux lifetime

`Pipeline` holds an opaque `NakiMux *`. Only `pipeline_output.c`, `pipeline_mux.c` and `pipeline_audio.c` see its representation. It contains the format context, exclusive local FD, header state, live interleaver state, output watermark, audio tracks/BSFs, deferred audio and protocol interrupt storage. Custom local AVIO callbacks receive this allocation, never a Pipeline address.

Allocation precedes any output or deferred-audio ownership. Protocol/header setup remains on the output owner. For the existing lazy destination activation, the original owner stops using the mux and transfers the same allocation to the new primary writer. The immutable encoder timing descriptor is prepared before that writer starts. Thread-start failure transfers the object back; neither success nor rollback copies tracks, FD ownership, clocks or callback state. After a successful transfer, only the writer accesses the object until it joins. The orchestration Pipeline no longer needs to remain alive merely to keep an output callback address valid; existing join ordering is still required for all other shared encoder/job/delivery references.

Close order is unchanged: drain if appropriate, trailer, AVIO flush, protocol/custom-FD close, publish each finalization error, then retire queued audio/BSF references. A destination allocates its mux before starting its thread and keeps the pointer and atomic interrupt storage stable through open, failure and completion. Only after join may the coordinator free that storage; concurrent cancellation can therefore still target a completed writer safely. `pipeline_mux_release` refuses to free an unclosed output. Normal empty-input and initialization-failure paths may have no mux. Cancellation requests an atomic I/O deadline and does not reclaim an active writer.

This preserves the current direct-to-writer handoff. It does not introduce a permanently running writer thread into every output, alter interleaving budgets, or provide new destination/codec combinations.

## Change and verification scope

| Change | Required focused checks |
| --- | --- |
| Dependencies/public headers/build targets | Architecture + organization + service build; control sources compile without SDK flags |
| Intent hashing/normalization | identity-intent + control-spec + update/recovery contracts; retain compatibility vectors |
| Mux allocation/handoff/close | mux-lifetime; lifecycle-media, output-finalize, rendition destination/socket regressions |
| Audio registration/clock ownership | audio-signatures, live-mux, live-input, rendition audio/SEI |
| Shared pool/exit proof | Native pool/wire/guard tests and rendition lifecycle/fault/recovery |
| Rendition membership/restart/operation phase | media-lifecycle + rendition-attach; rendition process/thread, recovery, churn and format/subscription regressions |
| Destination execution/admission | media-lifecycle + destination-runtime + mux-lifetime; rendition destinations/socket regressions |

`mux-lifetime` links the real output implementation. It exercises conflicting owners, rollback, stable protocol and AVIO callback addresses, writing after the former Pipeline has been freed, FD close and exactly-once deferred-reference release. It uses an isolated local MPEG-TS writer; independent decode and actual network behavior remain integration-test responsibilities.

Cross-cutting lifetime/ownership changes require full functional and application sanitizer regression. A local feature change can use the relevant focused checks once its dependency and ownership scope is established. Existing test suites remain authoritative; do not remove a boundary case merely because a module was reorganized. Performance and capacity claims require separate fresh measurements.

## Worker publication boundary

`Job` is opaque outside worker implementation. Media borrows immutable configuration/identity, retains existing lifetime-bound service handles, and publishes typed checkpoints, observations and completed-write events through `job_media.h`. It never takes the Job mutex or receives writable statistics/progress pointers. The worker owns every-frame latency sampling, first-I/recurring-I/output-key classification, reporting windows and final classification. Only the mux owner can publish completed writes; publication and factory callers are enumerated by the architecture checker.

After media references retire, the shared pipeline, fixed-branch owner and rendition adapter publish completion directly to the worker. The checker enumerates these three callers; the output implementation cannot publish job completion during mux finalization. Reader scheduling declarations are internal to Pipeline; `pipeline_reader.h` exposes only the bounded item and its release contract to wire consumers.

`job_remote.h` keeps remote slot allocation, child/destination factories and status projection inside worker implementation. Slot allocation rolls back a partially allocated batch; factory roles reject a mismatched destructor. Adapters still prove holder exit/thread join before retiring jobs. Compiler checks reject `sizeof(Job)`, direct field access and external mutex access; native tests cover observations, bounded admission, cancellation and allocation rollback.

## Lifecycle authority

`NakiRenditionLife` is an inline opaque value, with separate membership, holder, handshake, stop-policy and outcome dimensions. Those dimensions represent legitimate independent facts rather than unrestricted boolean combinations. Candidate reservation does not create logical membership; attachment requires the current generation, successful prepare and a running healthy holder. Removal drops membership immediately while retaining resources/ID until exit proof. Restart detaches the logical member, waits for the old holder, then begins a strictly newer generation. Failure is sticky until that new incarnation; clearing a reaped candidate's ID preserves its failure evidence for the pending operation. Abort cannot downgrade to drain.

Topology advances through checked `RESERVED → WAIT_EXIT → PREPARING → WAIT_FRAME` transitions (add omits WAIT_EXIT). A reservation/old holder readiness cannot trigger attachment. The existing source-format, subscription epoch, idle deadline and frame-identity barriers remain required. Prepared/applied operation commits are checked before membership changes; the separate actual-write checkpoint remains authoritative for delivery.

Destinations use an atomic writer execution value and a distinct owner-serialized attachment value. Concurrent ready/fail/finish events preserve terminal completion, sticky failure and stop escalation. New admission requires writer readiness, a verified AVC IDR and committed operation checkpoints; it does not use a generic I/key flag as a substitute. A finished message or writer-done flag is not resource exit proof: native join must succeed before the owner publishes reaped/joined and reuses ID/budget. Child startup/done and transport EOF remain one-way checkpoints, not independent topology flags.

The architecture guard restricts lifecycle mutation APIs to their actual native/topology owners, rejects direct opaque-storage access, and checks the three component build source sets. The pure `media-lifecycle` test has no media SDK dependency; it traverses 454 reachable rendition states and 8,172 event attempts, checks stale/late events, failed-candidate retirement, terminal/IDR/join rules and topology barriers, and races writer ready/fail/finish. Real media tests still verify the adapter wiring, ordered messages, raw leases and protocol behavior.

These constraints cover the completion scope in `architecture-completion.md`. They prevent accidental dependency and ownership drift; they do not eliminate runtime fault cases, justify removing integration coverage, or make this experimental service production-ready.

## Cleanup validation (2026-10-29)

The subtractive cleanup removes unused stream-description fields and the completion forwarding function, localizes reader declarations/includes, and shares 32 native-test configurations in the existing CMake file. No tracked files were added. Evidence is in `out/engineering-cleanup-20261029-013719/verification.json`: 33 native tests, six focused normal steps, five focused ASan/UBSan steps, and real destination/lifetime checks passed with source digest `ae51623f492cae93bf78e44b81a333450a62781632e541d775eeedcc2cf119c9`. All 64 target compile configurations and 33 CTest contracts match the prior build; the destination unit's deliberate link change reuses real release/identity helpers and traps unexpected I/O. Eighty historical documents remain byte-identical; 19 scratch tools were archived with byte verification, retaining validation scripts and evidence at their original locations. This cleanup does not add performance or capacity evidence.
