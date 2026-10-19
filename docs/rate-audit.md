# Optional constrained-CRF reservation audit

`NAKI_RATE_AUDIT=1` enables an output-owner observer for experimental constrained
CRF jobs. It is off unless the environment value is exactly `1`. It changes no
QP, model, budget, timestamps, pacing or encoder options. No SDK change is needed.
Use a complete existing trace (`network_trace`, `codec_trace`, or `frame_log`
with `frame_log_every=1`); missing complete trace configuration invalidates capture.

Each completed frame retains a monotonic output-local decision sequence and
reserve/accept/AU-feedback/write event numbers, encoder PTS, requested QP,
predicted/actual kinds and pending snapshots. Existing frame identity/generation
and codec bytes remain in the same frame record. An explicit I records, before
its QP selection, the previous partial group's last accepted unencoded frame by
ledger PTS and decision sequence, plus a P-model shadow reservation. This only
describes the fork's fixed GOP4 live-PTS explicit-I contract; it does not claim
an encoder ACK or predict arbitrary encoder layouts. EOF promotion is separate.

The bounded 256-entry observer checks phase transitions without controlling the
media path. Missing/duplicate identities or exhausted audit capacity increment
capture errors. At output completion it emits bounded outstanding entries and
one summary, including pending conservation and logger loss/error counters.
Nonzero capture errors, missing sequences/events, sampled detail, logger losses
or incomplete terminal records invalidate an audit. On failed/cancelled jobs,
outstanding reservations are reported rather than pretended to be written.
Logger submission retains its existing bounded, nonblocking behavior.

`rc_audit_boundary_status` is 0 for an empty group, 1 for an identified
unencoded target, 2 for a complete group, 3 when the last accepted frame has
already returned its AU, and -1 for unknown identity. EOF records a separate
event/group/target and never masquerades as a known I boundary. Successful
acceptance event numbers drive group position; source frame IDs and mux order
do not. A reserved send that fails remains phase 1; accepted/unencoded is phase
2 and received/not-written is phase 3 in terminal outstanding records.

The observer does add bounded CPU/memory/logging overhead when enabled. Since
the existing controller uses real wall time, a capture is not a promise of
bit-identical output across separate runs. It is not a quality fix or an
encoder feedback protocol. Original predictions remain immutable; this
capture does not replace reservations with its shadow estimate.

## Isolated three-picture reservation candidate (review only)

In this candidate worktree only, `NAKI_RATE_RESERVE_THREE=1` additionally opts
in to one behavior change. It requires `NAKI_RATE_AUDIT=1`, constrained CRF,
explicit source I, the fork's non-reference live-PTS GOP4 profile and future
guard 4. Both flags default off. It has not been tested on media or released.
Audit schema 2 records immutable `predicted_bits`, current `reserved_bits` and
one optional upgrade event with its trigger-I sequence and pending snapshots.

Before selecting the known I's budget, only an accepted three-picture partial
group's last predicted B may have its reservation raised to the existing
P-model shadow estimate. Its exact accepted sequence, PTS, phase2 state,
requested QP and original prediction must match both ledger and audit entry.
The accepted frame's QP/type prediction and model coefficients stay unchanged.
Complete groups, lengths 1/2, already-feedback, unknown and EOF targets are not
corrected. Missing prerequisites skip correction; numeric/invariant failures
leave controller state untouched and invalidate the audit.

Selection reserves `reserved_bits` (initially prediction); upgrade adds only
`new-old`; AU feedback replaces `actual-reserved`; successful write subtracts
actual. The ledger decision, frame snapshots and audit outstanding amount are
updated together. The upgrade precedes I selection and sits outside EAGAIN
retries. A failed send leaves that I selected/phase1, never accepted; raised
outstanding amounts remain recorded rather than being silently rolled back.

This is a review candidate, not a quality fix. More reservation can raise the
next I's QP and harm its reference chain. Unit/syntax checks and simulated
retry/failure tests do not establish real codec timing, bitrate or quality.
The observer's structure growth and read/write calls also have overhead even
when default off; no zero-overhead or performance benefit is claimed.
