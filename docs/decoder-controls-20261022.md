# Independent decoder controls: first implementation batch

**实验版，最终验收未完成，不建议生产启用**

This experimental branch adds actual synchronous and asynchronous decoder
backends. It does not remove the hardened H264 setup deferral or claim restored
frame parallelism. The historical archive remains immutable.

| Initialization field | Omitted | Explicit behavior |
| --- | --- | --- |
| `decoder_async` | true, existing async owner | false uses ordinary send/receive on the pipeline owner; calls can block for decoding |
| `decoder_auto_threads` | true, existing auto controller | false uses fixed `max_threads` on async; ignored for synchronous decoding |
| `decoder_early_output` | existing HEVC on / H264 off | independent request for eligible codec output paths; false selects original DPB |
| `decoder_pump` | false | existing dedicated async frontend; requires `decoder_async=true` |

All are initialization-only. Explicit true values for the first two fields keep
the old normalized fingerprint; false values change it. The four async x early
configurations have distinct identities. The service reports the selected backend,
effective auto-controller enablement and **requested** early-output override.
Requesting early output does not prove that any frame qualified for it.

For a fixed-thread comparison use `decoder_auto_threads=false`, identical
`max_threads`, `decoder_pump=false`, and vary only `decoder_async` and
`decoder_early_output`. The synchronous backend still uses FFmpeg frame threads.
Only the owning frontend calls either backend; packet EAGAIN ownership, NULL/EOF
draining, filter/encoder/mux ownership and cleanup remain on the same paths.
The async auto budget is created only for async + automatic operation.

The local readiness branch pins
`487ae0cc1fd307037c7f81812592f3db785d747f`, based on the published validation
snapshot `2673319b815fa6da6fccd0c3801e099b2e473066`. The follow-up avoids temporary
staging when the AVC plan only contains the ordinary selected frame. This is a
local dependency change; it has not replaced an installed production SDK or
global FFmpeg. The main integration history is recorded separately.
The fast-output algorithm still has the phase3 HRD/CFR, single-slice eligibility
restrictions and no-HRD fallback. No offline verified-even assertion is supplied
by the service.

Sampled completed-frame traces add:

- `decoder_async`, effective `decoder_auto_threads`;
- first submit-attempt and receive-return monotonic timestamps;
- `decoder_submit_retry_ms`: last submit-attempt start minus first attempt start;
- `decoder_submit_to_receive_ms`: receive return minus last attempt start;
- `decoder_receive_to_prepare_ms`: metadata preparation timestamp minus receive return.

Retry time does not include the final send call duration. Submit-to-receive
includes output ordering and backend scheduling, not just pixel computation.
These fields do not measure worker reference/setup waits separately. Existing
ingress-to-mux measurements still end after actual local write/flush. None of
these are source-to-screen or wire-time measurements.

## Checks and remaining work

`decoder-controls` is a native quick CTest checking defaults, types, invalid
pump combinations, fingerprint compatibility and branch inheritance. The
`naki-decoder-backend-probe INPUT EXPECTED_FRAMES` fixture uses the **same backend
helpers as the service** and requires three complete replay cycles with ordered
pixel/PTS/source-ID/metadata agreement for sync, async-fixed and async-auto x
early off/on. Only early-on pkt_dts is excepted. It is not a performance test.
An optional third argument sets a positive replay-cycle count (maximum 10,000)
for longer drain/flush/replay and sanitizer runs; omitting it still selects three.

The `decoder-controls` full integration suite starts a real controller/worker,
checks all four fixed-thread configurations, frame/source ordering, actual
decoded output frame count, final status fields and trace timing sanity. It
also runs the backend probe. It accepts existing input and a declared expected
count, or generates a small HRD B3 functional fixture before starting the service:

```sh
bash scripts/test-service.sh full --suite decoder-controls
# Explicit existing-fixture entry, with a fresh result directory:
python3 tests/integration/decoder_controls_integration.py \
  --input /lab/hrd-b3.mp4 --expected-frames 192 --lifecycle --results /lab/out/UNIQUE
```

The registered full suite enables `--lifecycle --include-pump`: in addition to
the four basic combinations it checks async + pump with early output off/on.
Every configuration first
decodes a paced prefix, stops while input remains, verifies its terminal state
and ordered unique output prefix, then must restart and produce the complete
declared output. Cancelled prefixes need not have equal length across backends.

The pump uses its own nonblocking CLOEXEC eventfd. Producers notify without
taking the pump's mutex; the pump drains the event before checking work, so a
notification arriving before poll remains readable. Reader progress, decoder
completion, lane release and join explicitly wake it. The one-second wait is
for stats refresh, replacing the former 2ms missed-condition-notification poll.
The fd stays alive until the reader has joined and async_free has quiesced all
callbacks. Non-pump modes do not allocate this extra fd.

The local paced comparison tool uses existing, hash-bound inputs with declared
frame counts, fixed threads/queues and reversed paired order on alternate
repeats. It checks actual decoded output and source/display identities before
reporting exact trace percentiles. Its queue snapshots are sampled, CPU is not
measured, and its post-accept terminal duration is not a sustained capacity test:

```sh
python3 tests/benchmarks/decoder_controls_latency.py \
  --manifest /lab/input-manifest.json --results /lab/out/UNIQUE \
  --threads 4 --repeats 2 --include-pump
```

Manifest shape: `{"frames_per_cycle":192,"inputs":[{"case":"hrd-b3",
"path":"/lab/hrd-b3.mp4","sha256":"..."}]}`. This command never generates
inputs. It validates the checkout SDK and actual loaded library paths/hashes,
and writes a failed summary on incomplete execution. See the
[local readiness report](decoder-local-readiness-20261022.md).

Functional success does not establish sustained queue stability or latency SLOs.
The current batch's platform-specific execution results are in the accompanying
[validation report](validation-decoder-controls-20261022.md). Pending work includes H264 output/decode state separation,
specialized malformed-AU/OOM/thread review of that redesign, realistic sustained
load/CPU/queue age and complete-service P95/P99 comparisons. Keep current
defaults while those gates are incomplete.
