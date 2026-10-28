# Media ownership and maintenance

Read `docs/architecture.md` for the implemented boundaries and test selection.

- Keep public headers free of `worker/job.h`, `pipeline_internal.h`, `mux_internal.h`, and `rendition_runtime_internal.h`. Use forward declarations and explicit dependencies. Private include consumers are enumerated by `scripts/check-architecture.py`.
- `Pipeline` is orchestration state, not a place to add shared component internals. New mux/audio/protocol state belongs inside `NakiMux`; no other component may define, inspect, copy or free that object. Its implementation is `pipeline_output.c`, `pipeline_mux.c`, and `pipeline_audio.c`, compiled together as `naki-media-output`.
- `pipeline_mux_move` is an exclusive transfer between quiescent owners of the same Job. Move the same allocation; do not clone it. Prepare immutable stream timing before starting a writer. A failed thread start returns the same object before releasing the candidate. Once started, join the writer before destroying its Pipeline. Finalize closes I/O; release then frees audio references and callback storage exactly once.
- Never introduce a second output packet-writing path, an independent audio-registration path, or an interleaver behind another component's back. Keep original first-frame, successful-write latency, watermark delivery and PTS-ledger confirmation boundaries.
- Frame wire codecs serialize explicit bounded fields and must not include mutable Job/Pipeline state. `frame_trace.h` is local lineage data, not a raw wire representation. Do not change its propagation/retirement semantics while reorganizing code.
- Lifecycle changes must state which owner advances the transition, what remains owned on failure, how cancellation wakes progress, and what proves a resource can be reclaimed. Extend deterministic contract tests first; retain the relevant real-media integration coverage.
