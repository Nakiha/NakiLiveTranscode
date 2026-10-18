#!/usr/bin/env python3
"""Real VVC bitstreams: variable PTS, changing cadence, gaps and every short tail."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import pathlib
import signal
import subprocess
import tempfile
import time

from support.service_harness import ServiceHarness
service = ServiceHarness()
from support.vvc_structure import decode_frames, check_pts_structure
from support.media_probe import PROBE, probe


def run():
    run=pathlib.Path(tempfile.mkdtemp(prefix="vvenc-pts-",dir=service.ROOT/"out"))
    log=(run/"server.jsonl").open("w"); error=(run/"server.stderr").open("w")
    state_directory=tempfile.TemporaryDirectory(prefix="naki-vvenc-pts-state-")
    server=subprocess.Popen([str(service.BIN/"naki-live"),"--port",str(service.PORT),"--run-dir",str(run/"logs"),"--state-dir",state_directory.name],
                            stdout=log,stderr=error)
    results=[]
    cases=[("cadence",105,"if(lt(N,40),N*3000,120000+(N-40)*6000)","hevc",10),
           ("jitter",145,"N*3600+floor(N/9)*900","h264",8),
           ("gaps",105,"N*3600+gte(N,12)*18000+gte(N,62)*54000","hevc",10),
           ("skip_periods",105,"N*3600+gte(N,20)*630000","h264",8),
           ("offset",105,"450000+N*3600+floor(N/17)*300","h264",8),
           *[(f"tail_{n}",n,"N*3600","h264",8) for n in (2,3,4,5,50,51,52)]]
    try:
        for _ in range(100):
            try: service.http("/health"); break
            except OSError: time.sleep(.03)
        else: raise AssertionError("server did not start")
        for name,count,expression,codec,depth in cases:
            fmt="mpegts" if name in ("gaps","skip_periods") else "mp4"
            source=run/f"{name}.mp4"; output=run/f"{name}-vvc.{('ts' if fmt=='mpegts' else 'mp4')}"
            vf="settb=1/90000,setpts="+expression.replace(",",r"\,")
            command=["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=25",
                     "-frames:v",str(count),"-vf",vf,"-enc_time_base","1:90000","-fps_mode","passthrough",
                     "-pix_fmt","yuv420p10le" if depth==10 else "yuv420p","-c:v","libx265" if codec=="hevc" else "libx264",
                     "-preset","ultrafast","-threads","2"]
            command+=(["-x265-params","pools=2:frame-threads=2:bframes=3:b-adapt=0:keyint=25:scenecut=0:log-level=error"]
                       if codec=="hevc" else ["-x264-params","bframes=3:b-adapt=0:keyint=25:scenecut=0"])
            subprocess.run(command+[str(source)],check=True,stderr=subprocess.DEVNULL,timeout=30)
            original=json.loads(subprocess.check_output(["ffprobe","-v","error","-select_streams","v:0","-show_frames",
                "-show_entries","frame=pts_time","-of","json",str(source)]))["frames"]
            times=[float(f["pts_time"]) for f in original]
            assert len(times)==count
            accepted=service.start({"input":str(source),"output":str(output),"output_format":fmt,
                                    "encoder":"libvvenc","frame_log":True})
            state=service.finished(timeout=40)
            assert state["packets_out"]==state["frames_decoded"]==state["frames_filtered"]==count,state
            decoded=decode_frames(PROBE,output)
            structure=check_pts_structure(decoded,times)
            packets=probe(output)["packets"]
            dts=[float(p["dts_time"]) for p in packets]
            assert all(a<b for a,b in zip(dts,dts[1:])),dts
            assert all(float(p["pts_time"])>=float(p["dts_time"]) for p in packets),packets
            # Independently compare decoded output PTS with trace mappings.
            trace=service.check_trace(state,count,source,reordered=count>2)
            assert len({f["pts_us"] for f in trace})==count
            key_trace=sorted((f for f in trace if f["output_key"]),key=lambda f:f["pts_us"])
            for row,late in zip(key_trace,structure["key_lateness_seconds"]):
                assert abs(row["keyframe_lateness_ms"]-late*1000)<.025,row
            assert key_trace[0]["previous_key_interval_ms"] is None
            for row,interval in zip(key_trace[1:],structure["key_period_seconds"]):
                assert abs(row["previous_key_interval_ms"]-interval*1000)<.025,row
            assert state["output_key_stages"]["encode"]["count"]==len(structure["key_frame_indices"])
            row={"case":name,"job_id":accepted["job_id"],"frames":count,"source_pts":times,
                 "structure":structure,"startup":state["startup"],"dts":dts}
            results.append(row)
            (run/f"{name}.state.json").write_text(json.dumps(state,indent=2))
            (run/"results.json").write_text(json.dumps(results,indent=2))
            print("PASS PTS GOP",name,structure["key_frame_indices"],structure["display_types"],flush=True)
    finally:
        server.send_signal(signal.SIGTERM); server.wait(timeout=15); log.close(); error.close();state_directory.cleanup()
    assert server.returncode==0
    (run/"COMPLETE.json").write_text(json.dumps({"status":"passed","cases":len(results)}))
    print("PASS PTS-grid CRA, VFR DTS, no lost frames, independent VVC decode:",run,flush=True)


if __name__=="__main__": run()
