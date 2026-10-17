#!/usr/bin/env python3
"""Real display-order I/PTS contracts and an independent blocked-mux watchdog."""
import json
import os
import pathlib
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time

import service_integration as service
from vvc_structure import decode_frames
from vvenc_integration import PROBE, CACHE


def command(args, timeout=45):
    try: return subprocess.run(list(map(str,args)),check=True,capture_output=True,text=True,timeout=timeout)
    except subprocess.CalledProcessError as error:
        raise AssertionError((list(map(str,args)),error.stderr[-4000:])) from error


def refresh_packets(path, encoder):
    """Inspect coded NAL types; decoder key flags also include recovery SEI."""
    result=command([PROBE,"-v","error","-select_streams","v:0","-show_packets","-show_data",
                    "-show_entries","packet=data,pts_time","-of","json",path])
    refresh=[]
    for i,packet in enumerate(json.loads(result.stdout)["packets"]):
        data=bytes.fromhex("".join(line.split(":",1)[1].strip().split("  ",1)[0].replace(" ","")
                                 for line in packet["data"].splitlines() if ":" in line))
        if data.startswith((b"\0\0\1",b"\0\0\0\1")):
            nals=[nal for nal in re.split(b"\x00\x00(?:\x00)?\x01",data) if nal]
        else:
            nals=[]; offset=0
            while offset<len(data):
                size=int.from_bytes(data[offset:offset+4],"big"); offset+=4
                assert size>0 and offset+size<=len(data),(i,size,len(data))
                nals.append(data[offset:offset+size]); offset+=size
        types=[nal[0]&31 if encoder=="libx264" else (nal[1]>>3)&31 for nal in nals]
        if any(typ==5 if encoder=="libx264" else typ in (7,8,9,10) for typ in types):
            refresh.append(i)
    return refresh


def trace(state):
    prefix=pathlib.Path(state["log_path"]); deadline=time.monotonic()+5
    while list(prefix.parent.glob(prefix.name+".*.active")) and time.monotonic()<deadline:
        time.sleep(.02)
    assert not list(prefix.parent.glob(prefix.name+".*.active"))
    return [json.loads(line) for file in sorted(prefix.parent.glob(prefix.name+".*.jsonl"))
            for line in file.read_text().splitlines()]


def expected_intra(frames, mode, interval):
    times=[round(float(row["pts_time"])*1000000) for row in frames]
    result=[]; anchor=times[0]; previous=anchor; grid=0
    for i,(row,pts) in enumerate(zip(frames,times)):
        source=row["pict_type"]=="I"
        due=(mode=="periodic_pts_grid" and (pts-anchor)//interval>grid or
             mode=="follow_source_with_max_gap" and pts-previous>=interval)
        if not i or due or (source and mode!="periodic_pts_grid"):
            result.append(i); previous=pts; grid=(pts-anchor)//interval
    return result


def check_vvc_intra_groups(decoded):
    """Each ordinary I closes the preceding GOP4 group, including short tails."""
    types="".join(frame["pict_type"] for frame in decoded)
    intras=[i for i,typ in enumerate(types) if typ=="I"]
    assert intras and intras[0]==0,types
    for begin,end in zip(intras,intras[1:]+[len(types)]):
        expected="I"; remaining=end-begin-1
        while remaining:
            size=min(4,remaining)
            expected+="B"*(size-1)+"P"; remaining-=size
        assert types[begin:end]==expected,(begin,end,types[begin:end],expected)
    return types


def rewrite_pes_clock(source, output, mode):
    data=bytearray(source.read_bytes()); previous=None; pictures=0
    def get(p):
        return ((p[0]>>1)&7)<<30 | p[1]<<22 | (p[2]>>1)<<15 | p[3]<<7 | p[4]>>1
    def put(start, value):
        value&=(1<<33)-1
        data[start:start+5]=bytes([(data[start]&0xf0)|((value>>30)&7)<<1|1,
            (value>>22)&255,((value>>15)&127)<<1|1,(value>>7)&255,(value&127)<<1|1])
    for start in range(0,len(data),188):
        assert data[start]==0x47
        if not data[start+1]&0x40: continue
        control=(data[start+3]>>4)&3
        if not control&1: continue
        p=start+4+(1+data[start+4] if control&2 else 0)
        if p+19>start+188 or data[p:p+3]!=b"\0\0\1" or not 0xe0<=data[p+3]<=0xef: continue
        flags=(data[p+7]>>6)&3
        if not flags&2: continue
        pts=get(data[p+9:p+14])
        if mode=="wrap":
            put(p+9,pts+(1<<33)-90000)
            if flags==3: put(p+14,get(data[p+14:p+19])+(1<<33)-90000)
        elif pictures==40:
            if mode=="missing":
                data[p+7]&=0x3f
                data[p+9:p+9+data[p+8]]=b"\xff"*data[p+8]
            else: put(p+9,previous-(3600 if mode=="backward" else 0))
        previous=pts; pictures+=1
    assert pictures==200,(mode,pictures)
    output.write_bytes(data)


class Receiver:
    def __init__(self, output):
        self.socket=socket.socket(); self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,16384)
        self.socket.bind(("127.0.0.1",0)); self.socket.listen(1); self.socket.settimeout(15)
        self.port=self.socket.getsockname()[1]; self.ready=threading.Event(); self.read=threading.Event()
        self.closed=threading.Event(); self.error=None; self.bytes=0; self.output=output
        self.thread=threading.Thread(target=self.receive); self.thread.start()

    def receive(self):
        try:
            connection,_=self.socket.accept()
            with connection, self.output.open("wb") as file:
                connection.settimeout(.2); self.ready.set()
                assert self.read.wait(15),"receiver release timed out"
                while not self.closed.is_set():
                    try: data=connection.recv(65536)
                    except socket.timeout: continue
                    if not data: break
                    self.bytes+=len(data); assert self.bytes<=128*1024*1024,"bounded capture overflow"
                    file.write(data)
        except Exception as error:
            self.error=repr(error)
        finally:
            self.socket.close()

    def finish(self):
        self.read.set(); self.thread.join(timeout=10)
        if self.thread.is_alive(): self.closed.set(); self.thread.join(timeout=3)
        assert not self.thread.is_alive() and not self.error,self.error


def run():
    run=pathlib.Path(tempfile.mkdtemp(prefix="frame-policy-",dir=service.ROOT/"out"))
    stdout=(run/"server.jsonl").open("w"); stderr=(run/"server.stderr").open("w")
    argv=[str(service.BIN/"naki-live"),"--port",str(service.PORT),"--run-dir",str(run/"logs"),"--status-interval","1"]
    server=subprocess.Popen(argv,stdout=stdout,stderr=stderr)
    results=[]

    def ready():
        for _ in range(100):
            try: service.http("/health"); return
            except OSError: time.sleep(.03)
        raise AssertionError("server did not start")

    def save(name,state,extra=None):
        row={"case":name,"state":state,**(extra or {})}; results.append(row)
        (run/(name+".json")).write_text(json.dumps(row,indent=2))

    try:
        ready()
        base=run/"base.mp4"
        command(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=25",
                 "-frames:v","200","-c:v","libx264","-preset","ultrafast","-crf","14","-threads","2",base])
        sources=[]
        for name,expression in (("plain",None),("holes","N*3600+gte(N,60)*450000"),("offset","450000+N*3600")):
            source=run/(name+".mp4")
            encoded=run/(name+"-with-recovery.mp4")
            forced="0.52,1.24,1.36,9.2,12.88,12.92" if name=="holes" else "5.52,6.24,6.36,12.2,12.88,12.92" if name=="offset" else "0.52,1.24,1.36,7.2,7.88,7.92"
            timed=base
            if expression:
                timed=run/(name+"-timestamps.mp4")
                command(["ffmpeg","-v","error","-i",base,"-vf","settb=1/90000,setpts="+expression.replace(",",r"\,"),
                         "-enc_time_base","1:90000","-fps_mode","passthrough","-c:v","libx264","-preset","ultrafast",
                         "-x264-params","keyint=1000:scenecut=0","-threads","2",timed])
            args=[CACHE/"install/bin/ffmpeg","-v","error","-copyts","-i",timed,"-an","-c:v","libx264","-preset","ultrafast",
                  "-live-frame-intra","1","-force_key_frames",forced,"-bf","3",
                  "-x264-params","b-adapt=0","-threads","2","-enc_time_base","1:90000","-fps_mode","passthrough"]
            command(args+[encoded])
            command(["ffmpeg","-v","error","-copyts","-i",encoded,"-an","-c:v","copy",
                     "-bsf:v","filter_units=remove_types=6",source])
            frames=decode_frames(PROBE,source)
            assert len(frames)==200
            if name=="offset":assert abs(float(frames[0]["pts_time"])-5)<.00003,frames[0]
            assert any(f["pict_type"]=="I" and not f["key_frame"] for f in frames[1:]),name
            sources.append((name,source,frames))
        hevc=run/"hevc10-vfr.mp4"
        command(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=30000/1001",
                 "-frames:v","120","-vf",r"format=yuv420p10le,settb=1/90000,setpts=N*3003+floor(N/9)*450",
                 "-enc_time_base","1:90000","-fps_mode","passthrough","-c:v","libx265","-preset","ultrafast",
                 "-x265-params","keyint=17:min-keyint=17:scenecut=0:bframes=3:b-adapt=0:pools=2:frame-threads=2:log-level=error",hevc])
        sources.append(("hevc10-vfr",hevc,decode_frames(PROBE,hevc)))

        for source_name,source,original in sources:
            modes=("follow_source","periodic_pts_grid","follow_source_with_max_gap") if source_name=="plain" else ("periodic_pts_grid",) if source_name=="hevc10-vfr" else ("follow_source_with_max_gap",)
            for encoder in ("libx264","libvvenc"):
                for mode in modes:
                    name=f"{source_name}-{encoder}-{mode}"; output=run/(name+".mp4")
                    intra={"mode":mode,"refresh_type":"encoder_intra"}
                    if mode=="periodic_pts_grid": intra["period_ms"]=2000
                    if mode=="follow_source_with_max_gap": intra["max_gap_ms"]=3000
                    service.start({"input":str(source),"output":str(output),"encoder":encoder,"output_format":"mp4",
                                   "frame_log":True,"timestamps":{"mode":"source_all"},"intra":intra})
                    state=service.finished(timeout=60); decoded=decode_frames(PROBE,output)
                    wanted=expected_intra(original,mode,2000000 if mode=="periodic_pts_grid" else 3000000)
                    actual=[i for i,f in enumerate(decoded) if f["pict_type"]=="I"]
                    assert actual==wanted,(name,actual,wanted)
                    refresh=refresh_packets(output,encoder)
                    assert refresh==[0],(name,"ordinary I upgraded to IDR/CRA",refresh)
                    count=len(original)
                    assert len(decoded)==state["packets_out"]==state["frames_decoded"]==count,state
                    before=[float(f["pts_time"]) for f in original]; after=[float(f["pts_time"]) for f in decoded]
                    offset=after[0]-before[0]
                    assert abs(offset)<.00003,(name,"source_all shifted the origin",offset)
                    assert max(abs(a-b-offset) for a,b in zip(after,before))<.00003,(name,before,after)
                    assert state["intra_requested"]==state["intra_confirmed"]==len(wanted),state
                    records=sorted((r for r in trace(state) if r["event"]=="frame"),key=lambda f:f["frame_id"])
                    assert [r["frame_id"] for r in records]==list(range(1,count+1)),records
                    assert all(r["generation"]==1 and r["timestamp_provenance"]=="source_all" for r in records)
                    assert [i for i,r in enumerate(records) if r["intra_reason"]]==wanted
                    assert all(r["output_picture_type"]=="I" for r in records if r["intra_reason"])
                    assert [r["output_picture_type"] for r in records]==[f["pict_type"] for f in decoded]
                    if encoder=="libvvenc":
                        check_vvc_intra_groups(decoded)
                    save(name,state,{"expected_I":wanted,"actual_I":actual,"refresh_packets":refresh,"source_pts":before,"output_pts":after,
                                     "output_picture_types":"".join(f["pict_type"] for f in decoded)})
                    print("PASS ordinary I / source PTS",name,actual,flush=True)

        # FLV's final millisecond precision is checked in display order. The
        # strict source clock is increasing, but these retained pictures collide.
        fine=run/"submillisecond.mp4"
        command(["ffmpeg","-v","error","-i",base,"-frames:v","8","-vf","settb=1/90000,setpts=450000+N*30",
                 "-enc_time_base","1:90000","-fps_mode","passthrough","-c:v","libx264","-preset","ultrafast",
                 "-bf","0","-threads","2",fine])
        original_fine=decode_frames(PROBE,fine)
        source_times=[float(row["pts_time"]) for row in original_fine]
        assert len(source_times)==8 and all(a<b for a,b in zip(source_times,source_times[1:]))
        milliseconds=[round(pts*1000) for pts in source_times]
        assert any(a==b for a,b in zip(milliseconds,milliseconds[1:]))
        service.start({"input":str(fine),"output":str(run/"strict-collision.flv"),"output_format":"flv",
                       "timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source"}})
        state=service.finished(expected="failed",timeout=30)
        assert "strict output PTS precision collision" in state["error"],state
        save("strict-FLV-precision-collision",state,{"source_pts":source_times,"quantized_pts_ms":milliseconds})

        # A positive origin makes reordered DTS representable in FLV. Packet
        # PTS may decrease in encode order while display PTS remains increasing.
        offset_source=next((source,frames) for name,source,frames in sources if name=="offset")
        output=run/"strict-reordered.flv"
        service.start({"input":str(offset_source[0]),"output":str(output),"output_format":"flv","encoder":"libvvenc",
                       "timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source"}})
        state=service.finished(timeout=60); decoded=decode_frames(PROBE,output)
        packets=json.loads(command([PROBE,"-v","error","-show_packets","-show_entries","packet=pts_time","-of","json",output]).stdout)["packets"]
        packet_pts=[float(row["pts_time"]) for row in packets]
        assert any(a>b for a,b in zip(packet_pts,packet_pts[1:])),packet_pts
        display_pts=[float(row["pts_time"]) for row in decoded]
        source_pts=[float(row["pts_time"]) for row in offset_source[1]]
        assert len(display_pts)==len(source_pts)==200 and all(a<b for a,b in zip(display_pts,display_pts[1:]))
        assert all(abs(a-b)<.00051 for a,b in zip(display_pts,source_pts)),(display_pts,source_pts)
        save("strict-FLV-reordered-PTS",state,{"packet_pts":packet_pts,"display_pts":display_pts})

        ts=run/"clock.ts"
        command(["ffmpeg","-v","error","-i",base,"-an","-c:v","copy","-bsf:v","h264_mp4toannexb",
                 "-mpegts_copyts","1","-muxdelay","0","-muxpreload","0","-f","mpegts",ts])
        for mode in ("duplicate","backward","missing","wrap"):
            source=run/("clock-"+mode+".ts"); rewrite_pes_clock(ts,source,mode)
            output=run/("clock-"+mode+"-output.ts")
            service.start({"input":str(source),"output":str(output),"timestamps":{"mode":"source_all"},
                           "intra":{"mode":"follow_source"}})
            state=service.finished(expected="completed" if mode=="wrap" else "failed",timeout=30)
            if mode=="wrap":
                before=decode_frames(PROBE,source); after=decode_frames(PROBE,output)
                assert len(before)==len(after)==200
                assert max(abs(float(a["pts_time"])-float(b["pts_time"])) for a,b in zip(after,before))<.00003,(before,after)
            else:
                assert "strict source PTS" in state["error"] and state["frames_decoded"]>0,state
            save("clock-"+mode,state); print("PASS strict PES clock",mode,flush=True)

        service.start({"input":str(base),"output":str(run/"strict-negative-flv.flv"),"output_format":"flv",
                       "encoder":"libvvenc","timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source"}})
        state=service.finished(expected="failed",timeout=40)
        assert "strict FLV timestamp range" in state["error"],state
        save("strict-FLV-negative-preroll",state)

        raw=run/"missing-pts.h264"
        command(["ffmpeg","-v","error","-i",base,"-an","-c:v","copy","-bsf:v","h264_mp4toannexb","-f","h264",raw])
        service.start({"input":str(raw),"output":str(run/"missing-strict.ts"),"timestamps":{"mode":"source_all"}})
        state=service.finished(expected="failed"); assert "strict source PTS" in state["error"],state
        save("strict-missing-PTS",state)
        service.start({"input":str(raw),"output":str(run/"missing-legacy.ts")})
        state=service.finished(timeout=30); assert state["packets_out"]==200,state
        assert state["timestamps_mode"]=="legacy" and not state["overload_enabled"],state
        save("legacy-missing-PTS",state)
        # Reference keeps its existing nominal-time-base behavior only in legacy.
        service.start({"input":str(base),"output":str(run/"reference-legacy.mp4"),"encoder":"libvvenc",
                       "encoder_profile":"reference","timestamps":{"mode":"legacy"}})
        state=service.finished(timeout=60); assert state["packets_out"]==200,state
        save("legacy-reference-compatible",state)
        zero_recovery={"timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source"},
            "frame_selection":{"mode":"live_drop_only","overload":{"signal":"oldest_inflight_ingress_age_ms",
                "levels":[{"enter_ms":100,"sustain_ms":20,"keep_one_in":2}],"recover_below_ms":0,
                "recover_sustain_ms":30,"max_continuous_degraded_ms":1000,"on_exhausted":"fail_task"}}}
        for i,policy in enumerate(({"timestamps":{"mode":"source_i_anchors"}},
                                   {"timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source","refresh_type":"idr"}},
                                   {"frame_selection":{"mode":"live_drop_only"}},
                                   {"encoder":"libvvenc","encoder_profile":"reference","timestamps":{"mode":"source_all"}},
                                   {"encoder":"libvvenc","encoder_profile":"reference","timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source"}},
                                   zero_recovery)):
            service.http(data={"input":str(base),"output":str(run/f"invalid-{i}.ts"),**policy},code=400)

        # A small advertised receive window and a large bounded noisy stream
        # block mux I/O. The worker control loop must still age live records.
        noisy=run/"noisy.mp4"
        command(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=512x288:rate=25",
                 "-vf","noise=alls=80:allf=t+u","-frames:v","320","-c:v","libx264","-preset","ultrafast",
                 "-crf","18","-g","19","-bf","0","-threads","2",noisy],timeout=60)
        noisy_frames=decode_frames(PROBE,noisy)
        source_i_ids={i+1 for i,row in enumerate(noisy_frames) if row["pict_type"]=="I"}
        planned_i_ids={i+1 for i in expected_intra(noisy_frames,"follow_source_with_max_gap",300000)}
        assert len(noisy_frames)==320 and planned_i_ids-source_i_ids
        for fail in (False,True):
            name="overload-exhausted" if fail else "overload-recovery"
            receiver=Receiver(run/(name+".ts"))
            policy={"mode":"live_drop_only","overload":{"signal":"oldest_inflight_ingress_age_ms",
                "levels":[{"enter_ms":10 if fail else 150,"sustain_ms":0,"keep_one_in":2},
                          {"enter_ms":50 if fail else 300,"sustain_ms":0,"keep_one_in":3}],
                "recover_below_ms":5 if fail else 50,"recover_sustain_ms":100 if fail else 200,
                "max_continuous_degraded_ms":300 if fail else 15000,
                "on_exhausted":"fail_task"}}
            try:
                accepted=service.start({"input":str(noisy),"output":f"tcp://127.0.0.1:{receiver.port}","bitrate":12000000,"pace":not fail,
                               "frame_log":True,"timestamps":{"mode":"source_all"},"intra":{"mode":"follow_source_with_max_gap","max_gap_ms":300},
                               "frame_selection":policy})
                assert receiver.ready.wait(10)
                if fail:
                    # Kill only this test's controller. The verified existing
                    # worker must enforce overload even while disconnected.
                    server.kill(); server.wait(timeout=10); time.sleep(1.2)
                    server=subprocess.Popen(argv,stdout=stdout,stderr=stderr); ready()
                    state=service.finished(expected="failed",timeout=15)
                    assert "continuous overload budget exhausted" in state["error"],state
                    assert state["overload_state"]=="failed",state
                    assert state["job_id"]==accepted["job_id"] and state["overload_fail_write_in_progress_ms"]>0,state
                else:
                    time.sleep(3.5); receiver.read.set()
                    state=service.finished(timeout=45)
                    assert state["frames_dropped_overload"]>0 and state["overload_escalations"]>0,state
                    assert state["overload_recoveries"]>0 and state["overload_level"]==0,state
                    assert state["packets_out"]+state["frames_dropped_overload"]==state["frames_decoded"]==320,state
                save(name,state); print("PASS independent overload watchdog",name,flush=True)
            finally: receiver.finish()
            if not fail:
                decoded=decode_frames(PROBE,receiver.output)
                assert len(decoded)==state["packets_out"],(len(decoded),state)
                rows=trace(state); dropped=[r for r in rows if r["event"]=="frame_drop"]
                kept=[r for r in rows if r["event"]=="frame"]
                assert len(dropped)==state["frames_dropped_overload"] and len(kept)==state["packets_out"]
                kept=sorted(kept,key=lambda row:row["frame_id"])
                kept_ids={row["frame_id"] for row in kept}; dropped_ids={row["frame_id"] for row in dropped}
                assert kept_ids.isdisjoint(dropped_ids) and kept_ids|dropped_ids==set(range(1,321))
                assert source_i_ids<=kept_ids and planned_i_ids<=kept_ids,(source_i_ids-kept_ids,planned_i_ids-kept_ids)
                actual_i_ids={row["frame_id"] for row,picture in zip(kept,decoded) if picture["pict_type"]=="I"}
                assert actual_i_ids==planned_i_ids=={row["frame_id"] for row in kept if row["intra_reason"]}
                assert state["intra_confirmed"]==state["intra_requested"]==len(planned_i_ids),state
                (run/"overload-recovery-protection.json").write_text(json.dumps({"source_I_frame_ids":sorted(source_i_ids),
                    "due_I_frame_ids":sorted(planned_i_ids-source_i_ids),"actual_I_frame_ids":sorted(actual_i_ids),
                    "kept_frame_ids":sorted(kept_ids),"dropped_frame_ids":sorted(dropped_ids)},indent=2))
    finally:
        if server.poll() is None: server.send_signal(signal.SIGTERM); server.wait(timeout=15)
        stdout.close(); stderr.close()
    assert server.returncode==0,server.returncode
    (run/"COMPLETE.json").write_text(json.dumps({"status":"passed","cases":len(results)},indent=2))
    print("PASS complete frame policy integration:",run,flush=True)


if __name__=="__main__":run()
