#!/usr/bin/env python3
"""Real process isolation, warm/cold startup, first-frame trace and bounded shutdown."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import os
import pathlib
import resource
import signal
import statistics
import subprocess
import time
import tempfile
import urllib.request
from support.service_harness import ServiceHarness
service_client = ServiceHarness(prefix="process-")
ROOT = service_client.ROOT
BIN = service_client.BIN
http = service_client.http
start = service_client.start
finished = service_client.finished
port = service_client.port
import socket
from support.event_logs import read_events
from support.processes import wait_for, dead, stop
STATES={}




def ready():
    health=http("/health")
    return health if health["pool"]["ready"]>=1 else None




def serve(name, spares=1):
    if name not in STATES:STATES[name]=tempfile.mkdtemp(prefix="pool-",dir=os.environ.get("TMPDIR"))
    state=STATES[name]
    output=(RUN/f"{name}.jsonl").open("a")
    errors=(RUN/f"{name}.stderr").open("a")
    def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    server=subprocess.Popen([str(BIN/"naki-live"),"--port",str(PORT),"--run-dir",str(RUN/name),
                             "--state-dir",state,
                             "--spare-workers",str(spares),"--worker-timeout","2",
                             "--worker-stop-timeout","1","--status-interval","1"],
                            stdout=output,stderr=errors,cwd=ROOT,preexec_fn=no_core)
    output.close();errors.close()
    def online():
        assert server.poll() is None, (name,server.returncode)
        try:return http("/health")
        except OSError:return False
    wait_for(online)
    return server




def events(name):
    return read_events(RUN/name/"events")


def silent(name):
    return start({"input":f"udp://127.0.0.1:{port(socket.SOCK_DGRAM)}","output":str(RUN/f"{name}.ts")})


def run():
    global RUN, BASE, PORT
    RUN, BASE, PORT = service_client.RUN, service_client.BASE, service_client.PORT
    sample=RUN/"small.mp4"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25",
                    "-frames:v","25","-c:v","libx264","-threads","2","-bf","3",str(sample)],check=True,timeout=30)
    measurements={"warm":[],"cold":[]}
    server=serve("warm")
    try:
        health=wait_for(ready)
        idle_pid=next(w["pid"] for w in health["pool"]["workers"] if w["state"]=="ready")
        assert idle_pid!=server.pid and health["execution_mode"]=="process_pool"
        duplicate=subprocess.run([str(BIN/"naki-live"),"--port",str(port()),"--run-dir",str(RUN/"warm"),"--state-dir",STATES["warm"]],
                                  capture_output=True,timeout=3)
        assert duplicate.returncode!=0 and http("/health")["status"]=="ok"
        # Keep a task running while the supervisor replenishes its standby process.
        data=json.dumps({"input":str(sample),"output":str(RUN/"first.ts"),"pace":True}).encode()
        request=urllib.request.Request(BASE+"/v1/job",data=data,
                                       headers={"Content-Type":"application/json","X-Request-ID":"customer-first-frame"})
        with urllib.request.urlopen(request,timeout=3) as response:accepted=json.load(response)
        assert accepted["supervisor"]["worker_pid"]==idle_pid
        wait_for(ready)
        assert len(http("/health")["pool"]["workers"])==2
        http(data={"input":str(sample),"output":str(RUN/"busy.ts")},code=409)
        state=finished()
        assert state["startup"]["prewarmed"] is True and state["packets_out"]==25
        assert state["supervisor"]["exit_code"]==0 and not state["supervisor"]["active"]
        assert dead(idle_pid)
        rows=wait_for(lambda:[e for e in events("warm") if e.get("job_id")==state["job_id"] and e["event"]=="first_frame"])
        first=next(e for e in rows if e["first_video"])
        assert first["request_id"]=="customer-first-frame" and first["first_output_key"]==1
        stages=["request","dispatch","worker_received","packet_ingress","decoder_submit","decoded",
                "filter_submit","filtered","encoder_submit","encoded","mux_begin","mux_flush_done"]
        timestamps=[first[f"{stage}_monotonic_us"] for stage in stages]
        assert timestamps==sorted(timestamps), first
        assert abs(first["request_to_mux_ms"]-(timestamps[-1]-timestamps[0])/1000)<1e-6
        assert abs(first["request_to_mux_ms"]-state["startup"]["request_to_first_video_mux_ms"])<1e-6
        assert not list((RUN/"warm").glob("*.trace.*")) # default trace off still emits first_frame
        print("PASS warm process selection, immediate replenishment, one-job admission and unconditional first-frame chain",flush=True)

        # Native crash must leave the parent control plane and the next standby usable.
        wait_for(ready);job=silent("crash");pid=job["supervisor"]["worker_pid"]
        os.kill(pid,signal.SIGSEGV)
        failed=finished("failed",timeout=5)
        assert failed["supervisor"]["exit_signal"]==signal.SIGSEGV and http("/health")["status"]=="ok"
        wait_for(lambda:any(e["event"]=="worker_crash" and e.get("job_id")==job["job_id"] and
                            e["snapshot"]["supervisor"]["exit_signal"]==signal.SIGSEGV for e in events("warm")))
        wait_for(ready)
        assert any(e["event"]=="startup_incomplete" and e.get("job_id")==job["job_id"] for e in events("warm"))

        # Frozen worker: its heartbeat becomes stale, while parent API remains responsive.
        job=silent("heartbeat");pid=job["supervisor"]["worker_pid"];os.kill(pid,signal.SIGSTOP)
        began=time.monotonic()
        while time.monotonic()-began<.35:assert http("/health")["status"]=="ok"
        failed=finished("failed",timeout=5)
        assert "heartbeat timeout" in failed["error"] and failed["supervisor"]["exit_signal"]==signal.SIGKILL
        assert dead(pid)

        wait_for(ready);job=silent("forced-stop");pid=job["supervisor"]["worker_pid"]
        os.kill(pid,signal.SIGSTOP);began=time.monotonic()
        http("/v1/job/stop",data={},code=202)
        stopped=finished("stopped",timeout=3)
        assert time.monotonic()-began<2.5 and stopped["supervisor"]["exit_signal"]==signal.SIGKILL
        print("PASS native crash isolation, heartbeat timeout, stale worker stop deadline and reaping",flush=True)

        # Idle crashes replenish with backoff; no task is automatically replayed.
        idle=wait_for(ready);pid=next(w["pid"] for w in idle["pool"]["workers"] if w["state"]=="ready")
        os.kill(pid,signal.SIGKILL)
        wait_for(lambda:all(w["pid"]!=pid for w in http("/health")["pool"]["workers"]))
        wait_for(ready)
        assert http()["job_id"]==job["job_id"]
        for i in range(10):
            wait_for(ready)
            start({"input":str(sample),"output":str(RUN/f"warm-{i}.ts")})
            done=finished();assert done["startup"]["prewarmed"] is True
            measurements["warm"].append(done["startup"])
        print("PASS standby crash replenishment and ten warm starts",flush=True)
        # Real media continues for longer than the heartbeat timeout with no controller.
        wait_for(ready);udp_port=port(socket.SOCK_DGRAM)
        job=start({"input":f"udp://127.0.0.1:{udp_port}","output":str(RUN/"survive.ts")})
        pid=job["supervisor"]["worker_pid"]
        sender=subprocess.Popen(["ffmpeg","-v","error","-re","-stream_loop","-1","-i",str(sample),
                                 "-c","copy","-f","mpegts",f"udp://127.0.0.1:{udp_port}?pkt_size=1316"],
                                stdout=subprocess.DEVNULL,stderr=(RUN/"sender.stderr").open("w"))
        try:
            wait_for(lambda:http().get("packets_out",0)>20)
            before=http()["packets_out"];size_before=(RUN/"survive.ts").stat().st_size
            wait_for(ready)
            children=[w["pid"] for w in http("/health")["pool"]["workers"]]
            server.kill();server.wait(timeout=3)
            time.sleep(2.5)
            assert all(not dead(child) for child in children)
            assert (RUN/"survive.ts").stat().st_size>size_before
            server=serve("warm")
            recovered=wait_for(lambda:(r if (r:=http()).get("job_id")==job["job_id"] and r["supervisor"]["recovered"] else None))
            assert recovered["supervisor"]["worker_pid"]==pid and recovered["packets_out"]>before
            assert recovered["control_reconnects"]>=1 and recovered["control_disconnected_ms"]>2000
            assert recovered["first_frame_checkpoints"],recovered
            first=[e for e in events("warm") if e.get("job_id")==job["job_id"] and e["event"]=="first_frame" and e["first_video"]]
            assert len(first)==1 and not first[0].get("replayed_after_reconnect"),first
            http("/v1/job/stop",data={},code=202)
            done=finished("stopped")
            assert not done["supervisor"]["exit_status_known"] and dead(pid)
            print("PASS continued UDP transcode during control outage, same-PID takeover, status and stop after recovery",flush=True)
        finally:
            sender.terminate();sender.wait(timeout=5)

        # A worker which finishes while disconnected retains its terminal snapshot for takeover.
        wait_for(ready)
        job=start({"input":str(sample),"output":str(RUN/"offline-complete.ts"),"pace":True})
        pid=job["supervisor"]["worker_pid"]
        wait_for(lambda:http().get("packets_out",0)>=1)
        server.kill();server.wait(timeout=3);time.sleep(1.5)
        assert not dead(pid)
        server=serve("warm")
        wait_for(lambda:http().get("job_id")==job["job_id"])
        done=finished();assert done["packets_out"]==25 and done["supervisor"]["worker_pid"]==pid
        assert dead(pid)
        print("PASS offline completion retained and recovered without duplicate task start",flush=True)
    finally:stop(server)

    server=serve("cold",spares=0)
    try:
        assert http("/health")["pool"]["workers"]==[]
        for i in range(10):
            start({"input":str(sample),"output":str(RUN/f"cold-{i}.ts")})
            done=finished();assert done["startup"]["prewarmed"] is False
            measurements["cold"].append(done["startup"])
        silent("shutdown");began=time.monotonic();stop(server)
        assert time.monotonic()-began<3
    finally:stop(server)
    for name in ("warm","cold"):events(name) # entire output remains JSONL
    report={"boundary":"complete HTTP request handler to first video packet mux/flush; local file input, WSL, 10 runs per mode",
            "measurements":measurements,"summary":{}}
    for name,values in measurements.items():
        report["summary"][name]={field:{"median_ms":statistics.median(v[field] for v in values),
                                      "min_ms":min(v[field] for v in values),"max_ms":max(v[field] for v in values)}
                                  for field in ("request_to_dispatch_ms","dispatch_to_worker_ms","worker_boot_ms","request_to_first_video_mux_ms")}
    (RUN/"startup-measurements.json").write_text(json.dumps(report,indent=2))
    (RUN/"COMPLETE.json").write_text(json.dumps({"status":"passed","run":str(RUN)},indent=2))
    print("PASS ten cold starts, active SIGTERM shutdown and startup measurements; artifacts:",RUN,flush=True)


if __name__=="__main__":run()
