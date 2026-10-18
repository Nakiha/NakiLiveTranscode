#!/usr/bin/env python3
"""Real RTMP/Enhanced FLV and SRT/TS, late audio, silence and unchanged connections."""
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
from support.media_probe import PROBE, CACHE, inspect
from support.vvc_structure import decode_frames, check_pts_structure
from support.live_stream_fixtures import create

LIVE_TIMEOUT_SECONDS = 22


def check_file_audio(run):
    """Exercise EOF drain, MP3 passthrough and the explicit video-only option."""
    source=run/"mp3-input.ts"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=25:duration=3",
                    "-f","lavfi","-i","sine=frequency=660:sample_rate=44100:duration=3",
                    "-c:v","libx264","-preset","veryfast","-threads","2","-bf","3",
                    "-c:a","libmp3lame","-b:a","64k","-f","mpegts",str(source)],check=True,timeout=30)
    for fmt,audio_copy in (("mpegts",True),("flv",True),("mpegts",False),("mp4",True)):
        output=run/f"mp3-{fmt}-{int(audio_copy)}.{fmt}"
        service.start({"input":str(source),"output":str(output),"output_format":fmt,"encoder":"libvvenc",
                       "audio_copy":audio_copy,"frame_log":True})
        state=service.finished(timeout=25)
        copied=audio_copy and fmt!="mp4"
        assert state["packets_out"]==75 and state["mux_queued_packets"]==0,state
        assert state["audio_tracks"]==int(copied),state
        streams=inspect(output)
        assert [s["codec_name"] for s in streams]==(["vvc","mp3"] if copied else ["vvc"]),streams
        assert int(streams[0]["nb_read_frames"])==75,streams
        if copied:
            assert int(streams[1]["nb_read_frames"])>50 and int(streams[1]["sample_rate"])==44100,streams
        service.check_trace(state,75,reordered=True)
        (run/f"mp3-{fmt}-{int(audio_copy)}-state.json").write_text(json.dumps(state,indent=2))
    print("PASS MP3 passthrough, EOF drain, audio_copy=false and video-only MP4",flush=True)


def run():
    run=pathlib.Path(tempfile.mkdtemp(prefix="live-mux-",dir=service.ROOT/"out"))
    subprocess.run([str(service.BIN/"naki-live-mux-test")],check=True,timeout=5)
    # The progress bound covers audio absences while video keeps arriving.
    # Keep the sender's video longer than the entire observation deadline,
    # including on a slow runner that falls behind real-time input.
    ts,flv=create(run,CACHE/"install/bin/ffmpeg",duration=LIVE_TIMEOUT_SECONDS+2)
    # Fixture must really reveal new streams after probing, not predeclare silent ones.
    server_log=(run/"server.jsonl").open("w");error=(run/"server.stderr").open("w")
    server=subprocess.Popen([str(service.BIN/"naki-live"),"--port",str(service.PORT),"--run-dir",str(run/"logs")],
                            stdout=server_log,stderr=error)
    results=[]
    try:
        for _ in range(100):
            try:service.http("/health");break
            except OSError:time.sleep(.03)
        for protocol,source,fmt in [("srt",ts,"mpegts"),("rtmp",flv,"flv")]:
            input_port=service.port();output_port=service.port()
            input_url=(f"srt://127.0.0.1:{input_port}?mode=caller&latency=50000" if protocol=="srt"
                       else f"rtmp://127.0.0.1:{input_port}/live/source")
            output_url=(f"srt://127.0.0.1:{output_port}?mode=caller&latency=50000" if protocol=="srt"
                        else f"rtmp://127.0.0.1:{output_port}/live/output")
            captured=run/f"{protocol}-captured.{('ts' if fmt=='mpegts' else 'flv')}"
            sender_url=input_url.replace("mode=caller","mode=listener")
            receiver_url=output_url.replace("mode=caller","mode=listener")
            sender_log=(run/f"{protocol}-sender.log").open("w");recv_log=(run/f"{protocol}-receiver.log").open("w")
            sender=subprocess.Popen([str(service.BIN/"naki-live-transport-fixture"),"serve",sender_url,str(source),
                                     "125000" if protocol=="srt" else "flv"],stdout=sender_log,stderr=sender_log)
            receiver=subprocess.Popen([str(service.BIN/"naki-live-transport-fixture"),"capture",receiver_url,str(captured),"unused"],
                                      stdout=recv_log,stderr=recv_log)
            history=[]
            try:
                time.sleep(.15)
                accepted=service.start({"input":input_url,"output":output_url,"output_format":fmt,"encoder":"libvvenc",
                                        "frame_log":True,"mux_wait_ms":50})
                began=time.monotonic();deadline=began+LIVE_TIMEOUT_SECONDS;pid=None;first_video_tracks=None
                while time.monotonic()<deadline:
                    assert sender.poll() is None,("live source exited",protocol)
                    assert "SOURCE_STOPPED" not in (run/f"{protocol}-sender.log").read_text(),("live source stopped sending",protocol)
                    state=service.http();history.append({"t":time.monotonic()-began,"video":state["packets_out"],
                        "audio":state.get("audio_packets_out",0),"tracks":state.get("audio_tracks",0),"state":state["state"],
                        "write_phase":state.get("write_phase"),"write_ms":state.get("write_in_progress_ms")})
                    (run/f"{protocol}-history.json").write_text(json.dumps(history,indent=2))
                    current_pid=state["supervisor"]["worker_pid"]
                    if pid is None:pid=current_pid
                    assert pid==current_pid and state["job_id"]==accepted["job_id"],state
                    if state["packets_out"] and first_video_tracks is None:first_video_tracks=state.get("audio_tracks",0)
                    assert state["state"] not in ("failed","completed","stopped"),state
                    if state["packets_out"]>=170:
                        service.http("/v1/job/stop",data={},code=202)
                        state=service.finished("stopped",timeout=8)
                        break
                    time.sleep(.05)
                else:raise AssertionError(("stream timed out",state))
                assert first_video_tracks==0,history
                assert state["audio_tracks"]==2 and state["audio_packets_out"]>50,state
                assert state["packets_out"]>=170,state
                receiver.wait(timeout=5)
                sender.terminate();sender.wait(timeout=5)
                sender_log.close();recv_log.close()
                assert (run/f"{protocol}-sender.log").read_text().count("CONNECTED_ONCE")==1
                assert (run/f"{protocol}-receiver.log").read_text().count("CONNECTED_ONCE")==1
                streams=inspect(captured)
                assert [s["codec_name"] for s in streams]==["vvc","aac","aac"],streams
                assert int(streams[0]["nb_read_frames"])>=151,streams
                assert all(int(s["nb_read_frames"])>20 for s in streams[1:]),streams
                decoded=decode_frames(PROBE,captured)
                # The retained video is byte-for-byte unchanged in the source fixture.
                source_frames=json.loads(subprocess.check_output([str(PROBE),"-v","error","-select_streams","v:0",
                    "-show_frames","-show_entries","frame=pts_time","-of","json",str(source)]))["frames"]
                # Cancellation can omit reordered tail B pictures; verify the
                # complete first three GOPs plus the fourth key independently.
                structure=check_pts_structure(decoded[:151],[float(f["pts_time"]) for f in source_frames[:151]])
                time.sleep(.15)
                prefix=pathlib.Path(state["log_path"])
                trace=[json.loads(line) for path in sorted(prefix.parent.glob(prefix.name+".*.jsonl"))
                       for line in path.read_text().splitlines()]
                assert len(trace)==state["packets_out"] and len({f["input_sequence"] for f in trace})==len(trace)
                assert state["ingress_to_mux"]["count"]==len(trace)
                assert max(f["interleave_queue_ms"] for f in trace)<400,trace
                assert state["mux_timeout_releases"]>0 and state["mux_high_water_packets"]<=256,state
                # Long audio absences must not stop video output for a second.
                last=None;max_gap=0
                for h in history:
                    if h["video"] and (last is None or h["video"]>last["video"]):
                        if last:max_gap=max(max_gap,h["t"]-last["t"])
                        last=h
                assert max_gap<1,(max_gap,history)
                results.append({"protocol":protocol,"worker_pid":pid,"job_id":accepted["job_id"],"streams":streams,
                                "structure":structure,"max_video_progress_gap_s":max_gap})
                (run/f"{protocol}-state.json").write_text(json.dumps(state,indent=2))
                (run/f"{protocol}-history.json").write_text(json.dumps(history,indent=2))
                print("PASS",protocol,"VVC + two late AAC tracks, same worker and connections, bounded silence",flush=True)
            finally:
                for process in (sender,receiver):
                    if process.poll() is None:process.terminate()
                    process.wait(timeout=5)
                sender_log.close();recv_log.close()
        check_file_audio(run)
    finally:
        server.send_signal(signal.SIGTERM);server.wait(timeout=15);server_log.close();error.close()
    assert server.returncode==0
    (run/"COMPLETE.json").write_text(json.dumps({"status":"passed","results":results},indent=2))
    print("PASS live transport and dynamic audio integration:",run,flush=True)


if __name__=="__main__":run()
