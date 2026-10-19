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
