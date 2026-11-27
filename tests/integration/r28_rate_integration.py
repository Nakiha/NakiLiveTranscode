#!/usr/bin/env python3
"""Exercise real grouped rate feedback through HTTP, interleaving and short EOF."""
import json
import pathlib
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from support.service_harness import BIN, PROBE, ServiceHarness
from support.vvc_structure import decode_frames


def run():
    service = ServiceHarness("r28-rate-")
    run_dir = service.RUN
    source = run_dir / "source.mp4"
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                    "testsrc2=size=256x192:rate=30", "-f", "lavfi", "-i",
                    "sine=frequency=440:sample_rate=48000", "-frames:v", "65",
                    "-c:v", "libx264", "-preset", "ultrafast", "-threads", "2",
                    "-c:a", "aac", "-shortest", str(source)], check=True, timeout=30)
    with (run_dir / "server.log").open("w") as log:
        server = subprocess.Popen([str(BIN / "naki-live"), "--port", str(service.PORT),
                                   "--run-dir", str(run_dir / "logs")], stdout=log, stderr=log)
        try:
            for _ in range(100):
                try:
                    if service.http("/health")["vvenc_enabled"]:
                        break
                except OSError:
                    time.sleep(.03)
            for tier, slots, fmt in (("live", 4, "mpegts"), ("quality", 8, "mp4")):
                output = run_dir / ("output.ts" if fmt == "mpegts" else "output.mp4")
                request = dict(input=str(source), output=str(output), output_format=fmt,
                               encoder="libvvenc", encoder_profile="r28_" + tier,
                               rate_control="constrained_crf", crf=29, maxrate=900000,
                               encoder_parallel_frames=slots, frame_log=True, pace=True)
                if tier == "quality":
                    request["rate_static_gop_credit_pct"] = 50
                service.start(request)
                state = service.finished(timeout=45)
                assert state["rate_window_ms"] == 5000 and state["encoder_parallel_frames"] == slots
                assert state["rate_static_gop_credit_pct"] == request.get("rate_static_gop_credit_pct", 0)
                frames = service.check_trace(state, 65, reordered=True)
                assert all(0 <= f["encoder_future_frames"] <= 10 and
                           0 <= f["rc_actual_slice_qp"] <= 63 and
                           0 <= f["rc_qp"] <= 63 and f["rc_predicted_bits"] > 0 and
                           f["rc_effective_bps"] == request["maxrate"] for f in frames)
                assert [f["output_dts_us"] for f in frames] == sorted({f["output_dts_us"] for f in frames})
                decoded = decode_frames(PROBE, output)
                assert len(decoded) == 65
                assert [i for i, f in enumerate(decoded) if f["key_frame"]] == [0, 60]
                streams = json.loads(subprocess.check_output([str(PROBE), "-v", "error",
                    "-show_entries", "stream=codec_name", "-of", "json", str(output)]))["streams"]
                assert {s["codec_name"] for s in streams} == ({"vvc", "aac"} if fmt == "mpegts" else {"vvc"})
        finally:
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=15)
    assert server.returncode == 0
    print("PASS R28 HTTP rate feedback, reordering, short GOP, audio and global headers", run_dir)


if __name__ == "__main__":
    run()
