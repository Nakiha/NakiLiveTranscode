#!/usr/bin/env python3
"""Exercise the real C service, HTTP client, media output and trace correlation."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import concurrent.futures
import json
import math
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

from support.service_harness import ServiceHarness

service = ServiceHarness()
ROOT, BIN = service.ROOT, service.BIN
http, start, finished = service.http, service.start, service.finished
port, probe = service.port, service.probe
check_duration, check_trace = service.check_duration, service.check_trace

def run():
    RUN, PORT, BASE = service.RUN, service.PORT, service.BASE
    sample = RUN / "h264-b3.mp4"
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=960x540:rate=25",
                    "-frames:v", "75", "-c:v", "libx264", "-preset", "veryfast", "-threads", "2",
                    "-x264-params", "bframes=3:b-adapt=0:keyint=25:scenecut=0", str(sample)], check=True, timeout=30)
    server_log = (RUN / "server.jsonl").open("w")
    server = subprocess.Popen([str(BIN / "naki-live"), "--port", str(PORT),
                               "--run-dir", str(RUN / "logs"), "--status-interval", "1",
                               "--log-rotate-bytes", "4096"], cwd=ROOT, stdout=server_log,
                              stderr=(RUN / "server.stderr").open("w"))
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise AssertionError("server exited")
            try:
                http("/health")
                break
            except OSError:
                time.sleep(0.03)
        else:
            raise AssertionError("server did not start")
        assert http()["state"] == "idle"
        http(data={}, code=400)
        http(raw=b'{"input":"x","input":"y","output":"z"}', code=400)
        http(raw=b'{"input":"x","output":"z","width":3,"height":4}', code=400)
        http(raw=b'{"input":"x","output":"z","extra":1}', code=400)
        http(raw=b'[]', code=400)
        http("/missing", code=404)
        http("/health", method="POST", code=405)
        client = subprocess.run([str(BIN / "naki-live-ctl"), "--url", BASE, "health"],
                                 capture_output=True, text=True, check=True)
        assert json.loads(client.stdout)["status"] == "ok"
        output = RUN / "file.ts"
        config = {"input": str(sample), "output": str(output), "width": 640, "height": 360, "pace": True, "frame_log": True}
        config_file = RUN / "job.json"
        config_file.write_text(json.dumps(config))
        subprocess.run([str(BIN / "naki-live-ctl"), "--url", BASE, "start", str(config_file)],
                        check=True, capture_output=True)
        # 202 proves worker acceptance. Operation-history reconciliation can
        # still gate unrelated admission with 503, especially under ASan.
        # Assert busy/409 only after the explicit controller readiness boundary;
        # never relax the service's recovery gate or retry a failed 409 check.
        deadline = time.monotonic() + 2
        while True:
            health = http("/health")
            if health["readiness"]["ready"]:
                assert health["admission"]["reason"] == "busy", health
                break
            assert health["readiness"]["reason"] == "worker_reconciliation_pending", health
            assert time.monotonic() < deadline, health
            time.sleep(0.005)
        http(data=config, code=409)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: http("/health"), range(24)))
        assert all(result["status"] == "ok" for result in results)
        state = finished()
        assert probe(output) == {"codec_name": "h264", "width": 640, "height": 360, "nb_read_frames": "75"}
        assert state["bytes_out"] == output.stat().st_size
        check_duration(output, 3.0)
        frames = check_trace(state, 75, sample)
        assert len(list(pathlib.Path(state["log_path"]).parent.glob(pathlib.Path(state["log_path"]).name+".*.jsonl")))>1
        assert all("scheduled_to_mux_ms" in row for row in frames)
        print("PASS file replay, B-frame correlation, 75 frames, scale/encode/mux, JSONL, concurrent HTTP", flush=True)

        # HEVC input and MP4 output exercise a second decoder and seekable custom AVIO.
        hevc = RUN / "hevc-b3.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
                        "-frames:v", "30", "-c:v", "libx265", "-preset", "ultrafast",
                        "-x265-params", "pools=2:frame-threads=2:bframes=3:b-adapt=0:log-level=error", str(hevc)],
                       check=True, timeout=30, stderr=subprocess.DEVNULL)
        mp4 = RUN / "hevc-output.mp4"
        start({"input": str(hevc), "output": str(mp4), "output_format": "mp4"})
        state = finished()
        assert probe(mp4)["nb_read_frames"] == "30"
        check_duration(mp4, 1.2)
        assert state["packets_out"]==30 and state["ingress_to_mux"]["count"]==30
        assert state["log_path"]=="" and state["frame_log"]==0
        print("PASS HEVC B-frame input, MP4 finalization, default frame logging disabled", flush=True)

        # A socket receiver independently captures the service's live UDP output.
        capture = RUN / "udp-captured.ts"
        receive_done = threading.Event()
        udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
        udp_socket.bind(("127.0.0.1", 0))
        udp_socket.settimeout(0.1)
        destination = udp_socket.getsockname()[1]
        def capture_packets():
            with capture.open("wb") as stream:
                while not receive_done.is_set():
                    try:
                        data = udp_socket.recv(65536)
                        stream.write(data)
                    except socket.timeout:
                        continue
        receiver = threading.Thread(target=capture_packets)
        receiver.start()
        try:
            start({**config, "output": f"udp://127.0.0.1:{destination}?pkt_size=1316"})
            state = finished()
            check_trace(state, 75, sample)
            assert state["network"]["udp_send"]["bytes"]>0
            time.sleep(0.15)
        finally:
            receive_done.set()
            receiver.join(timeout=2)
            udp_socket.close()
        assert probe(capture)["nb_read_frames"] == "75"
        print("PASS UDP output captured and independently decoded: 75 frames", flush=True)

        # Existing files cannot be overwritten, and failure does not kill the HTTP process.
        old = output.read_bytes()
        start(config)
        state = finished("failed")
        assert "File exists" in state["error"] and output.read_bytes() == old
        start({"input": str(RUN / "missing.mp4"), "output": str(RUN / "bad.ts")})
        finished("failed")
        http("/health")
        print("PASS bad input and exclusive output creation", flush=True)

        start({**config, "output": str(RUN / "cancel.ts")})
        time.sleep(0.3)
        http("/v1/job/stop", data={}, code=202)
        finished("stopped", timeout=5)
        print("PASS cancellation and job restart", flush=True)

        udp_port = port(socket.SOCK_DGRAM)
        udp_output = RUN / "udp.ts"
        start({"input": f"udp://127.0.0.1:{udp_port}?fifo_size=4096&overrun_nonfatal=0",
               "output": str(udp_output), "width": 640, "height": 360})
        sender = subprocess.Popen(["ffmpeg", "-v", "error", "-re", "-stream_loop", "-1", "-i", str(sample),
                                   "-map", "0:v:0", "-an", "-c:v", "copy", "-f", "mpegts",
                                   f"udp://127.0.0.1:{udp_port}?pkt_size=1316"],
                                  stderr=(RUN / "sender.stderr").open("w"))
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = http()
                assert state["state"] != "failed", state
                if state["packets_out"] >= 30:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError(("UDP did not produce frames", state))
            http("/v1/job/stop", data={}, code=202)
            state = finished("stopped", timeout=5)
            assert state["network"]["udp_receive"]["bytes"]>0
            assert state["network"]["udp_receive"]["background_calls"]>0
            assert int(probe(udp_output)["nb_read_frames"]) >= 30
            print("PASS real-time UDP input to H.264 MPEG-TS output and stop", flush=True)
        finally:
            sender.terminate()
            sender.wait(timeout=5)
        # Stop must also interrupt a network reader that has never received a packet.
        start({"input": f"udp://127.0.0.1:{port(socket.SOCK_DGRAM)}", "output": str(RUN / "silent.ts")})
        time.sleep(0.15)
        http("/v1/job/stop", data={}, code=202)
        finished("stopped", timeout=5)
        http("/health")
        print("PASS cancellation during silent UDP probe", flush=True)
    finally:
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
            raise
        server_log.close()
    assert server.returncode == 0, server.returncode
    for line in (RUN / "server.jsonl").read_text().splitlines():
        json.loads(line)
    (RUN / "COMPLETE.json").write_text(json.dumps({"status": "passed", "file_job": str(output),
                                                  "file_config": str(config_file), "run": str(RUN)}, indent=2))
    print("PASS SIGTERM shutdown; artifacts:", RUN, flush=True)


if __name__ == "__main__":
    run()
