#!/usr/bin/env python3
"""Test the built image and real Compose stack using isolated disposable volumes."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import os
import pathlib
import subprocess
import tempfile
import time
from support.event_logs import read_events

ROOT=pathlib.Path(__file__).resolve().parents[2]
RUN = None
PROJECT=f"naki-smoke-{int(time.time())}-{os.getpid()}"
ENV=dict(os.environ,NAKI_HTTP_PORT="0",NAKI_UDP_PORT="0")
COMPOSE=["docker","compose","-f",str(ROOT/"compose.yaml"),"-p",PROJECT]


def command(args,timeout=40):
    result=subprocess.run(args,cwd=ROOT,env=ENV,check=True,capture_output=True,text=True,timeout=timeout)
    with (RUN/"commands.log").open("a") as log:
        log.write(" ".join(args)+"\n"+result.stdout+result.stderr+"\n")
    return result.stdout.strip()


def control(action,*args):
    return json.loads(command(COMPOSE+["exec","-T","transcode","naki-live-ctl",action,*args]))


def run():
    global RUN
    RUN=pathlib.Path(tempfile.mkdtemp(prefix="docker-smoke-",dir=ROOT/"out"))
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=960x540:rate=25", "-frames:v","75",
                    "-c:v","libx264","-threads","2","-preset","veryfast","-bf","3",str(RUN/"input.mp4")],check=True,timeout=30)
    config={"input":"/data/input.mp4","output":"/data/output.ts","width":640,"height":360,"frame_log":True}
    (RUN/"job.json").write_text(json.dumps(config))
    started=False
    try:
        command(COMPOSE+["up","-d","--no-build","transcode"]);started=True
        container=command(COMPOSE+["ps","-q","transcode"])
        health=control("health")
        assert health["status"]=="ok" and health["status_interval_s"]==10 and health["network_observer_api"]==1
        for file in ("input.mp4","job.json"):command(COMPOSE+["cp",str(RUN/file),f"transcode:/data/{file}"])
        control("start","/data/job.json")
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            state=control("status")
            if state["state"] in ("completed","failed"):break
            time.sleep(.1)
        assert state["state"]=="completed" and state["packets_out"]==75,state
        assert state["logging"]["dropped_frames"]==0
        command(COMPOSE+["cp","transcode:/data/output.ts",str(RUN/"output.ts")])
        result=command(["ffprobe","-v","error","-select_streams","v:0","-count_frames","-show_entries",
                        "stream=width,height,nb_read_frames","-of","json",str(RUN/"output.ts")])
        stream=json.loads(result)["streams"][0]
        assert stream["width"]==640 and stream["height"]==360 and stream["nb_read_frames"]=="75"
        vvc_config={"input":"/data/input.mp4","output":"/data/output-vvc.mp4","output_format":"mp4",
                    "width":256,"height":144,"encoder":"libvvenc"}
        (RUN/"vvc.json").write_text(json.dumps(vvc_config))
        command(COMPOSE+["cp",str(RUN/"vvc.json"),"transcode:/data/vvc.json"])
        control("start","/data/vvc.json")
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            vvc_state=control("status")
            if vvc_state["state"] in ("completed","failed"):break
            time.sleep(.1)
        assert vvc_state["state"]=="completed" and vvc_state["packets_out"]==75,vvc_state
        assert vvc_state["encoder"]=="libvvenc" and vvc_state["startup"]["request_to_first_key_mux_ms"]>0
        command(COMPOSE+["cp","transcode:/data/output-vvc.mp4",str(RUN/"output-vvc.mp4")])
        (RUN/"vvc-state.json").write_text(json.dumps(vvc_state,indent=2))
        # Full independent VVC decode uses the SDK's n9 ffprobe in vvenc_integration;
        # the host distro ffprobe may predate VVC support.
        fault_config={"input":"udp://127.0.0.1:15999","output":"/data/worker-fault.ts"}
        (RUN/"worker-fault.json").write_text(json.dumps(fault_config))
        command(COMPOSE+["cp",str(RUN/"worker-fault.json"),"transcode:/data/worker-fault.json"])
        victim=control("start","/data/worker-fault.json")["supervisor"]["worker_pid"]
        command(COMPOSE+["exec","-T","transcode","python3","-c",
                         "import os,signal,sys; os.kill(int(sys.argv[1]),signal.SIGSEGV)",str(victim)])
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            failed=control("status")
            if failed["state"]=="failed":break
            time.sleep(.1)
        assert failed["state"]=="failed" and failed["supervisor"]["exit_signal"]==11,failed
        assert failed["supervisor"]["core_dumped"] is False
        # The container launcher restarts only the failed controller; the same worker survives.
        recovery={"input":"/data/input.mp4","output":"/data/recovery.ts","width":640,"height":360,"pace":True}
        (RUN/"recovery.json").write_text(json.dumps(recovery))
        command(COMPOSE+["cp",str(RUN/"recovery.json"),"transcode:/data/recovery.json"])
        accepted=control("start","/data/recovery.json")
        original_worker=accepted["supervisor"]["worker_pid"]
        original_controller=control("health")["supervisor_pid"]
        command(COMPOSE+["exec","-T","transcode","python3","-c",
                         "import os,signal,sys; os.kill(int(sys.argv[1]),signal.SIGSEGV)",str(original_controller)])
        deadline=time.monotonic()+15
        recovered=None
        while time.monotonic()<deadline:
            try:
                current=control("status")
                if current.get("job_id")==accepted["job_id"] and current["supervisor"]["recovered"]:
                    recovered=current
                    if current["state"]=="completed":break
            except subprocess.CalledProcessError:pass # HTTP may be absent briefly during restart.
            time.sleep(.1)
        assert recovered and recovered["state"]=="completed" and recovered["packets_out"]==75,recovered
        assert recovered["supervisor"]["worker_pid"]==original_worker
        assert control("health")["supervisor_pid"]!=original_controller
        assert json.loads(command(["docker","inspect",container]))[0]["RestartCount"]==0
        command(COMPOSE+["cp","transcode:/data/recovery.ts",str(RUN/"recovery.ts")])
        recovered_stream=json.loads(command(["ffprobe","-v","error","-select_streams","v:0","-count_frames",
                                             "-show_entries","stream=nb_read_frames","-of","json",str(RUN/"recovery.ts")]))["streams"][0]
        assert recovered_stream["nb_read_frames"]=="75"
        crash_dir=control("health")["crash_report_dir"]
        reports=json.loads(command(COMPOSE+["exec","-T","transcode","python3","-c",
                       "import json,pathlib,sys; print(json.dumps([json.loads(p.read_text()) for p in pathlib.Path(sys.argv[1]).glob('crash-*.json')]))",crash_dir]))
        assert all(any(r["pid"]==pid and r["role"]==role and r["capture_result"]=="captured" and
                       any(t["fault_thread"] and t["frames"] for t in r["threads"]) for r in reports)
                   for pid,role in ((victim,"worker"),(original_controller,"http"))),reports
        command(COMPOSE+["cp",f"transcode:{crash_dir}",str(RUN/"crashes")])
        if symbols_dir:=os.environ.get("NAKI_SYMBOLS_DIR"):
            for report_file in (RUN/"crashes").glob("crash-*.json"):
                report_data=json.loads(report_file.read_text())
                required="naki-live-worker" if report_data["role"]=="worker" else "naki-live"
                command(["python3",str(ROOT/"scripts/symbolize-crash.py"),str(report_file),
                         "--symbols",str(pathlib.Path(symbols_dir)/"app"),
                         "--symbols",str(pathlib.Path(symbols_dir)/"ffmpeg"),
                         "--symbols",str(pathlib.Path(symbols_dir)/"vvenc"),
                         "--require-module",required,"--output",str(report_file.with_suffix(".symbolized.txt"))])
        time.sleep(.2)
        command(COMPOSE+["run","--rm","--no-deps","log-maintenance","--directory","/var/log/naki","--min-age-seconds","0"])
        count=int(command(COMPOSE+["exec","-T","transcode","python3","-c",
                                  "import pathlib; print(len(list(pathlib.Path('/var/log/naki').glob('*.jsonl.gz'))))"]))
        assert count>=1
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            status=json.loads(command(["docker","inspect",container]))[0]
            if status["State"]["Health"]["Status"]=="healthy":break
            time.sleep(.3)
        assert status["State"]["Health"]["Status"]=="healthy"
        assert status["Config"]["User"]=="10001:10001"
        assert status["HostConfig"]["LogConfig"]["Type"]=="local"
        command(COMPOSE+["stop","transcode"])
        status=json.loads(command(["docker","inspect",container]))[0]
        assert status["State"]["ExitCode"]==0 and not status["State"]["OOMKilled"]
        lines=command(["docker","logs",container])
        (RUN/"container.jsonl").write_text(lines+"\n")
        events=[json.loads(line) for line in lines.splitlines()]
        command(["docker","cp",container+":/var/log/naki/events",str(RUN/"events")])
        process_events=read_events(RUN/"events")
        assert any(event["event"]=="shutdown" and event["role"]=="controller" for event in process_events)
        (RUN/"COMPLETE.json").write_text(json.dumps({"project":PROJECT,"status":"passed","frames":75,
                                                    "compressed_segments":count,"exit_code":0},indent=2))
        assert any(event["event"]=="control_restarting" for event in events)
        print("PASS Compose, worker/HTTP SIGSEGV stack reports without core, same-worker recovery, media, maintenance and SIGTERM:",RUN,flush=True)
    finally:
        # This project name is newly generated above; no pre-existing resources are targeted.
        if started:command(COMPOSE+["down","--volumes","--remove-orphans"])


if __name__=="__main__":run()
