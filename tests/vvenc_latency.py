#!/usr/bin/env python3
"""Compare real paced service profiles; persist bitstream, startup and stage evidence."""
import json
import argparse
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import service_integration as service
from vvenc_integration import PROBE
from vvc_structure import decode_frames, check_structure

ROOT, BIN = service.ROOT, service.BIN


def run(repeats=2):
    run=pathlib.Path(tempfile.mkdtemp(prefix="vvenc-latency-",dir=ROOT/"out"))
    source=run/"input.mp4"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25","-frames:v","175",
                    "-c:v","libx264","-preset","ultrafast","-threads","2","-x264-params",
                    "bframes=3:b-adapt=0:keyint=25:scenecut=0",str(source)],check=True,timeout=30)
    log=(run/"server.jsonl").open("w"); error=(run/"server.stderr").open("w")
    server=subprocess.Popen([str(BIN/"naki-live"),"--port",str(service.PORT),"--run-dir",str(run/"logs"),
                             "--status-interval","1"],stdout=log,stderr=error)
    rows=[]
    try:
        for _ in range(100):
            try: service.http("/health"); break
            except OSError: time.sleep(.03)
        for profile in ("reference","live_3b_2s","live_3b_2s_no_mctf"):
            for repeat in range(repeats):
                output=run/f"{profile}-{repeat}.ts"
                accepted=service.start({"input":str(source),"output":str(output),"encoder":"libvvenc",
                                        "encoder_profile":profile,"width":256,"height":144,"pace":True,"frame_log":True})
                state=service.finished(timeout=45)
                assert state["packets_out"]==175,state
                trace=service.check_trace(state,175,source,reordered=True)
                decoded=decode_frames(PROBE,output)
                assert len(decoded)==175,len(decoded)
                structure=check_structure(decoded,50) if profile!="reference" else {}
                (run/f"{profile}-{repeat}.state.json").write_text(json.dumps(state,indent=2))
                (run/f"{profile}-{repeat}.frames.json").write_text(json.dumps(decoded,indent=2))
                events=[json.loads(line) for line in (run/"server.jsonl").read_text().splitlines()]
                first=next(e for e in events if e["event"]=="first_frame" and e["job_id"]==accepted["job_id"])
                ready=next(e for e in events if e["event"]=="pipeline_ready" and e["job_id"]==accepted["job_id"])
                row={"profile":profile,"repeat":repeat,"startup":state["startup"],"first_output":first,
                     "encode":state["encode"],"encode_submit":state["encode_submit"],"encode_wait":state["encode_wait"],
                     "ingress_to_mux":state["ingress_to_mux"],"i_frame_stages":state["i_frame_stages"],
                     "output_key_stages":state["output_key_stages"],
                     "frame_types":state["frame_types"],"resolved_config":ready,"structure":structure}
                rows.append(row)
                (run/"results.json").write_text(json.dumps(rows,indent=2))
                print(profile,repeat,"first_ms",state["startup"]["request_to_first_video_mux_ms"],
                      "first_encode_ms",first["encode_ms"],"first_submit_ms",first["encode_submit_ms"],
                      "first_wait_ms",first["encode_wait_ms"],flush=True)
        # A real UDP source separates network open/probe time from encoder wait.
        source_port=service.port(socket.SOCK_DGRAM)
        output=run/"live-input.ts"
        accepted=service.start({"input":f"udp://127.0.0.1:{source_port}?fifo_size=4096&overrun_nonfatal=0",
                                "output":str(output),"encoder":"libvvenc","width":256,"height":144,"frame_log":True})
        sender=subprocess.Popen(["ffmpeg","-v","error","-re","-stream_loop","-1","-i",str(source),
                                 "-an","-c:v","copy","-f","mpegts",f"udp://127.0.0.1:{source_port}?pkt_size=1316"],
                                stdout=subprocess.DEVNULL,stderr=(run/"sender.stderr").open("w"))
        try:
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                state=service.http()
                assert state["state"] not in ("failed","completed","stopped"),state
                if state["packets_out"]>=65: break
                time.sleep(.03)
            else: raise AssertionError("live input did not produce VVC output")
            service.http("/v1/job/stop",data={},code=202)
            state=service.finished("stopped",timeout=8)
        finally:
            sender.terminate(); sender.wait(timeout=5)
        decoded=decode_frames(PROBE,output)
        assert len(decoded)==state["packets_out"]
        structure=check_structure(decoded,50)
        (run/"live-input.state.json").write_text(json.dumps(state,indent=2))
        (run/"live-input.structure.json").write_text(json.dumps(structure,indent=2))
        print("live UDP input first_ms",state["startup"]["request_to_first_video_mux_ms"],
              "probe_done_ms",state["startup"]["input_probe_done_ms"],flush=True)
    finally:
        server.send_signal(signal.SIGTERM); server.wait(timeout=15); log.close(); error.close()
    assert server.returncode==0
    (run/"COMPLETE.json").write_text(json.dumps({"status":"passed","results":"results.json"}))
    print("PASS paced VVenC profile comparison and 3B/50-frame key period:",run,flush=True)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats",type=int,choices=range(1,6),default=2)
    run(parser.parse_args().repeats)
