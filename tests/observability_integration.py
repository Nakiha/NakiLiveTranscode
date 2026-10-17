#!/usr/bin/env python3
"""Windowed latency, I pictures, blocked sinks, network hooks and log retention."""
import functools
import gzip
import http.server as http_server
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import threading
import time
import tempfile
from service_integration import ROOT, BIN, RUN, BASE, PORT, http, start, finished
from event_logs import read_events


def listen():
    sock=socket.socket()
    sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
    sock.bind(("127.0.0.1",0)); sock.listen(1); sock.settimeout(10)
    return sock


def run():
    sample=RUN/"sample.mp4"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=960x540:rate=25",
                    "-frames:v","150","-c:v","libx264","-preset","veryfast","-threads","2",
                    "-x264-params","bframes=3:b-adapt=0:keyint=25:scenecut=0","-movflags","+faststart",str(sample)],check=True,timeout=30)
    server_log=(RUN/"server.jsonl").open("w")
    state_directory=tempfile.mkdtemp(prefix="obs-",dir=os.environ.get("TMPDIR"))
    server=subprocess.Popen([str(BIN/"naki-live"),"--port",str(PORT),"--run-dir",str(RUN/"traces"),
                             "--state-dir",state_directory,
                             "--log-rotate-bytes","2048","--log-rotate-seconds","1","--status-interval","1"],
                            stdout=server_log,stderr=(RUN/"server.stderr").open("w"),cwd=ROOT)
    try:
        for _ in range(100):
            if server.poll() is not None:raise AssertionError("server exited")
            try:http("/health");break
            except OSError:time.sleep(.03)
        start({"input":str(sample),"output":str(RUN/"sample.ts"),"pace":True,
               "frame_log":True,"frame_log_every":20,"slow_frame_ms":600000})
        state=finished()
        assert state["ingress_to_mux"]["count"]==150
        assert state["frame_types"]["first_i"]["count"]==1
        assert state["frame_types"]["recurring_i"]["count"]>=4
        prefix=pathlib.Path(state["log_path"])
        for _ in range(100):
            traces=sorted(prefix.parent.glob(prefix.name+".*.jsonl"))
            if traces and not list(prefix.parent.glob(prefix.name+".*.active")):break
            time.sleep(.02)
        frames=[json.loads(line) for p in traces for line in p.read_text().splitlines()]
        assert len(traces)>1 and 0<len(frames)<150
        assert sum(row["decoded_picture_type"]=="I" for row in frames)==state["frame_types"]["i_frame"]["count"]
        assert sum(bool(row["output_key"]) for row in frames)==state["frame_types"]["output_key"]["count"]
        events=read_events(RUN/"traces/events")
        windows=[r for r in events if r.get("job_id")==state["job_id"] and r["event"]=="status"]
        assert len(windows)>=5 and sum(r["window"]["ingress_to_mux"]["count"] for r in windows)==150
        assert sum(r["window"]["frame_types"]["i_frame"]["count"] for r in windows)==state["frame_types"]["i_frame"]["count"]
        time.sleep(1.1)
        idle_events=read_events(RUN/"traces/events")
        assert any(r["event"]=="heartbeat" and r.get("last_job_id")==state["job_id"] for r in idle_events)
        print("PASS nonoverlapping latency windows, all-frame counts, separate first/recurring I frames, forced key-frame traces and rotation",flush=True)

        # Real TCP media output, independently received and finalized.
        listener=listen(); tcp_port=listener.getsockname()[1]; capture=RUN/"tcp.ts"
        def receive_tcp():
            conn,_=listener.accept()
            with conn,capture.open("wb") as output:
                while True:
                    data=conn.recv(65536)
                    if not data:break
                    output.write(data)
        receiver=threading.Thread(target=receive_tcp);receiver.start()
        start({"input":str(sample),"output":f"tcp://127.0.0.1:{tcp_port}"})
        tcp_state=finished();receiver.join(timeout=3);listener.close()
        assert not receiver.is_alive() and tcp_state["network"]["tcp_send"]["bytes"]==capture.stat().st_size>0

        class Handler(http_server.SimpleHTTPRequestHandler):
            def log_message(self,*args):pass
        web=http_server.ThreadingHTTPServer(("127.0.0.1",0),functools.partial(Handler,directory=RUN))
        web_thread=threading.Thread(target=web.serve_forever);web_thread.start()
        try:
            start({"input":f"http://127.0.0.1:{web.server_address[1]}/sample.mp4","output":str(RUN/"http.ts")})
            network_state=finished()
            assert network_state["network"]["tcp_receive"]["bytes"]>=sample.stat().st_size
        finally:web.shutdown();web.server_close();web_thread.join()
        print("PASS actual TCP send byte counts and HTTP/TCP receive observations",flush=True)

        udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(("127.0.0.1",0));udp.settimeout(.1)
        hold=threading.Event();received=[0]
        def receive_datagrams():
            while not hold.is_set():
                try:received[0]+=len(udp.recv(65536))
                except socket.timeout:pass
        receiver=threading.Thread(target=receive_datagrams);receiver.start()
        try:
            start({"input":str(sample),"output":f"udp://127.0.0.1:{udp.getsockname()[1]}?pkt_size=1316&bitrate=20000000"})
            datagrams=finished()
            assert datagrams["network"]["udp_send"]["background_calls"]>0 and received[0]>0
        finally:hold.set();receiver.join(timeout=2);udp.close()
        udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(("127.0.0.1",0));udp_port=udp.getsockname()[1];udp.close()
        start({"input":f"udp://127.0.0.1:{udp_port}?fifo_size=0","output":str(RUN/"udp-direct.ts")})
        sender=subprocess.Popen(["ffmpeg","-v","error","-re","-stream_loop","-1","-i",str(sample),
                                 "-map","0:v:0","-c","copy","-f","mpegts",f"udp://127.0.0.1:{udp_port}?pkt_size=1316"],
                                stderr=subprocess.DEVNULL)
        try:
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                direct=http()
                if direct["packets_out"]>=30:break
                assert direct["state"]!="failed",direct
                time.sleep(.1)
            else:raise AssertionError("direct UDP input did not produce frames")
            http("/v1/job/stop",data={},code=202);direct=finished("stopped",timeout=5)
            assert direct["network"]["udp_receive"]["bytes"]>0 and direct["network"]["udp_receive"]["background_calls"]==0
        finally:sender.terminate();sender.wait(timeout=5)
        print("PASS UDP background transmit and direct receive hook paths",flush=True)

        listener=listen(); hold=threading.Event()
        def blocked_receiver():
            conn,_=listener.accept()
            with conn:hold.wait(12)
        receiver=threading.Thread(target=blocked_receiver);receiver.start()
        start({"input":str(sample),"output":f"tcp://127.0.0.1:{listener.getsockname()[1]}",
               "width":1920,"height":1080,"bitrate":100000000})
        try:
            deadline=time.monotonic()+8
            while time.monotonic()<deadline:
                stalled=http()
                if stalled["write_in_progress_ms"]>500:break
                assert stalled["state"]!="failed",stalled
                time.sleep(.05)
            else:raise AssertionError(("no blocked write observed",stalled))
            http("/health")
            time.sleep(1.2)
            events=read_events(RUN/"traces/events")
            assert any(row.get("job_id")==stalled["job_id"] and row.get("write_in_progress_ms",0)>500 for row in events)
            http("/v1/job/stop",data={},code=202);finished("stopped",timeout=5)
        finally:hold.set();receiver.join(timeout=3);listener.close()
        print("PASS independent summaries and HTTP remain responsive during blocked network write",flush=True)
    finally:
        server.send_signal(signal.SIGTERM);server.wait(timeout=15);server_log.close()
    assert server.returncode==0

    # Native queue test deliberately blocks stdout before flooding trace records.
    subprocess.run([str(BIN/"naki-logger-test"),str(RUN/"bounded.trace")],check=True,timeout=10)

    # Socket readiness timeout preserves the original FFmpeg error even if the observer changes errno.
    listener=listen(); hold=threading.Event()
    def silent_peer():
        conn,_=listener.accept()
        with conn:hold.wait(5)
    peer=threading.Thread(target=silent_peer);peer.start()
    try:
        subprocess.run([str(BIN/"naki-network-probe"),f"tcp://127.0.0.1:{listener.getsockname()[1]}"],check=True,timeout=3)
    finally:hold.set();peer.join(timeout=3);listener.close()
    print("PASS network hook ordering and unchanged timeout/errno behavior",flush=True)

    # Only immutable sealed trace names are eligible for compression and deletion.
    directory=RUN/"traces"; sealed=list(directory.glob("*.jsonl"))
    original={p.name:p.read_bytes() for p in sealed}
    active=directory/"sentinel.trace.000001.jsonl.active";active.write_bytes(b"active\n")
    unrelated=directory/"unrelated.jsonl";unrelated.write_bytes(b"keep\n")
    link=directory/"link.trace.000001.jsonl";link.symlink_to(unrelated)
    expired=directory/"expired.trace.000001.jsonl.gz";expired.write_bytes(gzip.compress(b"expired\n"))
    old=time.time()-8*86400;os.utime(expired,(old,old))
    command=[sys.executable,str(ROOT/"scripts/maintain-logs.py"),"--directory",str(directory),"--min-age-seconds","0"]
    subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
    assert not expired.exists()
    for name,data in original.items():
        assert gzip.decompress((directory/(name+".gz")).read_bytes())==data
        assert not (directory/name).exists()
    subprocess.run(command+["--max-total-bytes","1"],check=True,stdout=subprocess.DEVNULL)
    assert not list(directory.glob("*.gz")) and active.read_bytes()==b"active\n" and unrelated.read_bytes()==b"keep\n" and link.is_symlink()
    print("PASS gzip content, size retention, active-file and symlink exclusion",flush=True)

    # Verify the real default interval, without overriding it for the accelerated tests.
    default_log=(RUN/"default.jsonl").open("w")
    default_server=subprocess.Popen([str(BIN/"naki-live"),"--port",str(PORT),"--run-dir",str(RUN/"default-traces"),
                                     "--state-dir",tempfile.mkdtemp(prefix="obs-",dir=os.environ.get("TMPDIR"))],
                                    stdout=default_log,stderr=subprocess.PIPE,cwd=ROOT)
    try:
        for _ in range(100):
            try:
                assert http("/health")["status_interval_s"]==10;break
            except OSError:time.sleep(.03)
        time.sleep(10.2)
    finally:default_server.send_signal(signal.SIGTERM);default_server.wait(timeout=5);default_log.close()
    assert default_server.returncode==0
    events=read_events(RUN/"default-traces/events")
    events=[e for e in events if e["role"]=="controller"]
    heartbeats=[e for e in events if e["event"]=="heartbeat"]
    assert len(heartbeats)==1 and 9.5<(heartbeats[0]["monotonic_us"]-events[0]["monotonic_us"])/1e6<11
    print("PASS default ten-second control timer; artifacts:",RUN,flush=True)


if __name__=="__main__":run()
