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

The experimental SDK gitlink points to the published phase3 snapshot
`2673319b815fa6da6fccd0c3801e099b2e473066` (phase3 codec source plus the active validation tools), making its H264 option available in
an explicitly rebuilt experiment. This is an experimental dependency change;
no default branch, installed production SDK or global FFmpeg has been changed.
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

The `decoder-controls` full integration suite starts a real controller/worker,
checks all four fixed-thread configurations, frame/source ordering, actual
decoded output frame count, final status fields and trace timing sanity. It
also runs the backend probe. It accepts existing input and a declared expected
count, or generates a small HRD B3 functional fixture before starting the service:

```sh
bash scripts/test-service.sh full --suite decoder-controls
# Explicit existing-fixture entry, with a fresh result directory:
python3 tests/integration/decoder_controls_integration.py \
  --input /lab/hrd-b3.mp4 --expected-frames 192 --results /lab/out/UNIQUE
```

Functional success does not establish sustained queue stability or latency SLOs.
The current batch's platform-specific execution results are in the accompanying
[validation report](validation-decoder-controls-20261022.md). Pending work includes H264 output/decode state separation,
specialized malformed-AU/OOM/thread review of that redesign, realistic sustained
load/CPU/queue age and complete-service P95/P99 comparisons. Keep current
defaults while those gates are incomplete.
