# Opt-in unified I/P/B reservation prototype

`NAKI_UNIFIED_RATE=1` enables this experimental worker-local path only for the
fixed single-output 960x540 VVenC selective-RDOQ2 constrained-CRF profile, source
rate limiting, and the unchanged 1,100,000-bit/s/future-4 contracts. The default
path is unchanged. No SDK/library change or release is included.

An admission ID is assigned once at the real filter-output boundary. At most
four distinct OPEN/PLANNED/TRYING plus accepted-without-AU frames exist across
application and codec. Startup I is sent immediately. Otherwise four non-I
frames close as BBBP; 1/2/3 non-I followed by source I close as P/BP/BBP plus I.
No subsequent batch is admitted until all current AUs return. Raw references,
stage bytes, shared IDs, consulted high-water marks and native future values are
recorded. This does not claim that unrelated input/decoder queues have no delay.

One joint lower-bound/proportional allocation uses known roles, historical
requested-QP coefficients, the existing activity upper bound 4, nominal priors
6/3.2/.2 and utilization .82. Only each own quota-to-QP conversion uses that
frame's activity. QP63 model minima plus known first configuration bytes are
preflight estimates, not real codec lower bounds or actual-AU guarantees.
The 95% rolling margin is soft: if its model preflight fails but the real four
headrooms fit, exactly one joint solve uses the real headroom and logs margin
use. No constant sweep or extra burst credit is used.

Quota belongs to C until successful send transfers it once into D. EAGAIN freezes
the current TRYING tuple and drains existing AUs; no-progress fails in bounded
steps. Actual replaces reserved debt; real flush moves debt into window/token
spending without creating credit. Early actual feedback jointly re-solves only
unfrozen plans. Source income uses bounded published completed-message records,
never mutable stream arrays; delayed publication cannot earn retroactive income
and retains original complete+1s expiry. Settlement uses actual flush time.
Real insufficiency allows one nonblocking progress/fresh-observation recheck,
then fails explicitly, retaining accepted debt and any already incurred breach.

True EOF closes a partial group, handles send(NULL) success/EAGAIN/error, and
marks NULL sent only on success. Cancellation releases unsubmitted references
and quotas, retains accepted debt and never invents EOF or writes. The original
filtered/ingress/network/HTTP startup timestamps remain intact; stage residence
is included in encoder_prepare and in the original ingress-to-mux and first
media-chunk read-to-real-flush measurements. Waiting can increase wall latency.

`naki-unified-rate-test` exercises the real C allocator/ledger and bounded codec
helper, including NULL EAGAIN/error/no-progress. `naki-unified-native-test` uses
the actual SDK with small synthetic 10-bit frames and the product ledger to
check startup, full/short groups without EOF/fifth input, genuine EOF, and
cancellation. Its write settlement is simulated; real mux/rate/latency/quality
claims require the separate RTMP paired experiments and exact-PTS quality audit.
Native NULL EAGAIN need not occur naturally; the explicit injected branch is
covered by the product helper test. Audit schema3 records all state mutations
and terminal outstanding debt. Logger loss invalidates evidence.

For this quality experiment alone `--latency-observe-only` retains all latency
statistics and 300ms violations but omits latency as an early rejection gate.
It leaves the four rate contracts, identity and future-frame limits enforced;
the default harness gate is unchanged. Accounting success is not a quality gain.
