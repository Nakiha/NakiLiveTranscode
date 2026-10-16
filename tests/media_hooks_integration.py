#!/usr/bin/env python3
"""Internal extension lifetime, copy isolation, encoded boundary and rejection."""
import json
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import time

import service_integration as service
from live_mux_integration import inspect


def run():
    run=pathlib.Path(tempfile.mkdtemp(prefix="media-hooks-",dir=service.ROOT/"out"))
    binaries=run/"bin";binaries.mkdir()
    for name in ("naki-live","naki-crash-collector"):
        shutil.copy2(service.BIN/name,binaries/name)
    shutil.copy2(service.BIN/"naki-live-hook-worker",binaries/"naki-live-worker")
    sample=run/"input.ts"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=25:duration=2.4",
                    "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=2.4",
                    "-c:v","libx264","-preset","veryfast","-threads","2","-bf","3",
                    "-c:a","aac","-ac","2","-f","mpegts",str(sample)],check=True,timeout=30)
    cases=[("valid","libx264","mpegts",True),("valid","libvvenc","mpegts",True),
           ("valid","libvvenc","mp4",True),("valid","libx264","mpegts",False),
           ("video_error","libvvenc","mpegts",True),("audio_error","libx264","mpegts",True),
           ("rewrite_pts","libx264","mpegts",True),("growth","libx264","mpegts",True),
           ("open_error","libx264","mpegts",True),
           ("cancel","libx264","mpegts",True)]
    for i,(mode,encoder,fmt,copy) in enumerate(cases):
        evidence=run/f"case-{i}-{mode}";evidence.mkdir()
        log=(evidence/"server.jsonl").open("w");error=(evidence/"server.stderr").open("w")
        server=subprocess.Popen([str(binaries/"naki-live"),"--port",str(service.PORT),"--run-dir",str(evidence/"logs")],
            stdout=log,stderr=error,env=dict(os.environ,NAKI_TEST_MEDIA_HOOK_DIR=str(evidence),NAKI_TEST_MEDIA_HOOK_MODE=mode))
        try:
            for _ in range(100):
                try:service.http("/health");break
                except OSError:time.sleep(.03)
            output=evidence/f"output.{fmt}"
            accepted=service.start({"input":str(sample),"output":str(output),"output_format":fmt,
                                   "encoder":encoder,"audio_copy":copy,"frame_log":True,"pace":mode=="cancel"})
            expected="completed" if mode=="valid" else "stopped" if mode=="cancel" else "failed"
            if mode=="cancel":
                deadline=time.monotonic()+8
                while time.monotonic()<deadline:
                    if service.http()["packets_out"]>=5:break
                    time.sleep(.03)
                else:raise AssertionError("no output before stop")
                service.http("/v1/job/stop",data={},code=202)
            state=service.finished(expected,timeout=20)
            report=json.loads((evidence/f"{accepted['job_id']}.json").read_text())
            assert report["cancelled"]==int(mode=="cancel"),report
            if mode=="valid":
                assert report["audio"]>50 and report["video"]==state["packets_out"]==60,report
                assert report["keys"]>=2 and report["error_code"]==0,report
                if encoder=="libvvenc":assert report["reordered_pts"]>0 and report["known_types"]==0,report
                else:assert report["known_types"]==60,report
                streams=inspect(output)
                copied=copy and fmt!="mp4"
                assert len(streams)==1+int(copied) and int(streams[0]["nb_read_frames"])==60,streams
                if copied:assert streams[1]["codec_name"]=="aac" and int(streams[1]["nb_read_frames"])>50,streams
                service.check_trace(state,60,reordered=encoder=="libvvenc")
            elif mode!="cancel":
                assert report["error_code"]<0 and "media hook" in state["error"],(report,state)
            (evidence/"state.json").write_text(json.dumps(state,indent=2))
            print("PASS media hooks",mode,encoder,fmt,"copy=",copy,flush=True)
        finally:
            server.send_signal(signal.SIGTERM);server.wait(timeout=15);log.close();error.close()
        assert server.returncode==0,server.returncode
    (run/"COMPLETE.json").write_text(json.dumps({"status":"passed","cases":len(cases)}))
    print("PASS serial hooks, borrowed refs, copy isolation, reorder/drain, errors and cancellation:",run,flush=True)


if __name__=="__main__":run()
