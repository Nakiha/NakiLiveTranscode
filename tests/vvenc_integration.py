#!/usr/bin/env python3
"""Real H.264/HEVC 8/10-bit -> VVC, independent decode, reordered trace and drain."""
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import threading
import time

import service_integration as service

ROOT, BIN = service.ROOT, service.BIN
RUN = pathlib.Path(tempfile.mkdtemp(prefix="vvenc-test-", dir=ROOT / "out"))
CACHE = pathlib.Path(os.environ.get("NAKI_BUILD_ROOT", pathlib.Path.home() / ".cache/naki-live-transcode-n9"))
PROBE = CACHE / "install/bin/ffprobe"


def probe(path):
    result = subprocess.run([str(PROBE), "-v", "error", "-threads", "2", "-select_streams", "v:0",
                             "-count_frames", "-show_packets", "-show_entries",
                             "stream=codec_name,pix_fmt,width,height,nb_read_frames:packet=pts_time,dts_time",
                             "-of", "json", str(path)], check=True, capture_output=True, text=True, timeout=40)
    return json.loads(result.stdout)


def run():
    log = (RUN / "server.jsonl").open("w")
    error = (RUN / "server.stderr").open("w")
    server = subprocess.Popen([str(BIN / "naki-live"), "--port", str(service.PORT), "--run-dir",
                               str(RUN / "logs"), "--status-interval", "1"], stdout=log, stderr=error)
    summaries = []
    try:
        for _ in range(100):
            try:
                assert service.http("/health")["vvenc_enabled"] == 1; break
            except OSError:
                time.sleep(.03)
        else:
            raise AssertionError("server did not start")
        for invalid in ({"encoder": "bad"}, {"encoder": "libvvenc", "encoder_preset": "veryfast"},
                        {"encoder": "libvvenc", "encoder_preset": "fast|medium"},
                        {"encoder": "libvvenc", "output_format": "flv"}):
            service.http(data={"input": "unused", "output": "unused", **invalid}, code=400)
        # Both decoder bit depths, resize and no resize, MPEG-TS and MP4, and short EOF.
        cases = [("h264", 8, "25", 175, "mpegts", True),
                 ("hevc", 10, "25", 75, "mp4", False),
                 ("h264", 10, "30000/1001", 65, "mpegts", True),
                 ("hevc", 8, "25", 1, "mp4", False)]
        for index, (codec, depth, rate, count, fmt, resize) in enumerate(cases):
            source = RUN / f"input-{index}.mp4"
            output = RUN / f"output-{index}.{'ts' if fmt == 'mpegts' else 'mp4'}"
            command = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=size=320x180:rate={rate}",
                       "-frames:v", str(count), "-pix_fmt", "yuv420p10le" if depth == 10 else "yuv420p",
                       "-c:v", "libx264" if codec == "h264" else "libx265", "-preset", "ultrafast", "-threads", "2"]
            if codec == "h264":
                command += ["-x264-params", "bframes=3:b-adapt=0:keyint=25:scenecut=0"]
            else:
                command += ["-x265-params", "pools=2:frame-threads=2:bframes=3:b-adapt=0:keyint=25:min-keyint=25:scenecut=0:log-level=error"]
            subprocess.run(command + [str(source)], check=True, stderr=subprocess.DEVNULL, timeout=30)
            cfg = {"input": str(source), "output": str(output), "output_format": fmt,
                   "encoder": "libvvenc", "frame_log": count > 1, "pace": index == 0}
            if resize:
                cfg.update(width=256, height=144)
            # Independently receive real VVC UDP output at fractional frame rate.
            done = threading.Event()
            receiver = None
            sock = None
            if index == 2:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
                sock.bind(("127.0.0.1", 0)); sock.settimeout(.05)
                cfg["output"] = f"udp://127.0.0.1:{sock.getsockname()[1]}?pkt_size=1316"
                def capture():
                    with output.open("wb") as stream:
                        while not done.is_set():
                            try:
                                stream.write(sock.recv(65536))
                            except socket.timeout:
                                pass
                receiver = threading.Thread(target=capture); receiver.start()
            try:
                accepted = service.start(cfg)
                if index == 0:
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        current = service.http()
                        if current.get("packets_out", 0) > 0:
                            assert current["frames_decoded"] < count, "VVC did not emit before input EOF"
                            (RUN / "streaming-before-eof.json").write_text(json.dumps(current, indent=2))
                            break
                        assert current["state"] not in ("completed", "failed"), current
                        time.sleep(.02)
                    else:
                        raise AssertionError("no live VVC output before EOF")
                state = service.finished(timeout=90)
                time.sleep(.15)
            finally:
                done.set()
                if receiver:
                    receiver.join(timeout=2); sock.close()
            decoded = probe(output)
            stream = decoded["streams"][0]
            assert stream == {"codec_name": "vvc", "width": 256 if resize else 320,
                              "height": 144 if resize else 180, "pix_fmt": "yuv420p10le", "nb_read_frames": str(count)}, stream
            assert state["encoder"] == "libvvenc" and state["encoder_preset"] == "faster"
            assert state["frames_decoded"] == state["frames_filtered"] == state["packets_out"] == count, state
            for name in ("demux_read", "input_queue", "decode", "setup", "filter", "encode", "mux",
                         "interleave_call", "avio_flush", "ingress_to_mux"):
                assert state[name]["count"] == count, (name, state[name])
            assert state["startup"]["request_to_first_video_mux_ms"] > 0
            if count > 1:
                frames = service.check_trace(state, count, source, reordered=True)
                assert any(a["pts_us"] > b["pts_us"] for a, b in zip(frames, frames[1:])), "reordering not exercised"
                first_i = [row for row in frames if row["first_i"]]
                assert len(first_i) == 1 and first_i[0]["pts_us"] == min(row["pts_us"] for row in frames if row["decoded_picture_type"] == "I")
                assert state["frame_types"]["recurring_i"]["count"] >= 1
                packets = decoded["packets"]
                dts = [float(row["dts_time"]) for row in packets]
                assert all(a < b for a, b in zip(dts, dts[1:])), dts
                expected_duration = (count - 1) * (1001 / 30000 if index == 2 else 1 / 25)
                assert abs((dts[-1] - dts[0]) - expected_duration) < .002, dts
            if index == 2:
                assert state["network"]["udp_send"]["bytes"] > 0
            summaries.append({"case": index, "input_codec": codec, "input_bits": depth, "frames": count,
                              "startup": state["startup"], "encode": state["encode"],
                              "i_frame_stages": state["i_frame_stages"]})
            (RUN / f"state-{index}.json").write_text(json.dumps(state, indent=2))
            print(f"PASS {codec}/{depth}-bit -> VVC/10-bit {fmt}, {count} frames, resize={resize}", flush=True)
        # Includes first-frame checkpoints even when per-frame logging is disabled.
        time.sleep(.1)
    finally:
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=15)
        log.close(); error.close()
    assert server.returncode == 0
    events = [json.loads(line) for line in (RUN / "server.jsonl").read_text().splitlines()]
    ready = [event for event in events if event["event"] == "pipeline_ready"]
    assert len(ready) == len(cases) and all("yuv420p10le" in event["filter"] for event in ready)
    for summary in summaries:
        state = json.loads((RUN / f"state-{summary['case']}.json").read_text())
        assert any(e["event"] == "first_frame" and e["job_id"] == state["job_id"] for e in events)
    (RUN / "COMPLETE.json").write_text(json.dumps({"status": "passed", "cases": summaries}, indent=2))
    print("PASS VVenC independent decode, PTS ledger through reorder/drain, first/I/all-stage metrics:", RUN, flush=True)


if __name__ == "__main__":
    run()
