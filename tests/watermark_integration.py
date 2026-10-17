#!/usr/bin/env python3
"""Bounded assets, real frame pixels/timeline and worker HTTP hot replacement."""
import binascii
import hashlib
import http.server as http_server
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("NAKI_SERVICE_BIN", ROOT / "out/bin"))
RUN = pathlib.Path(tempfile.mkdtemp(prefix="watermark-", dir=ROOT / "out"))


def command(args, timeout=30, env=None):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=timeout, env=env)
    assert p.returncode == 0, (args, p.returncode, p.stderr[-3000:])
    return p.stdout


def png(path, color, alpha=255, transparent_left=False):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\0" + b"".join(bytes((*color, 0 if transparent_left and x < 4 else alpha)) for x in range(8)) for _ in range(8))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def gif(path, zero=False, repeat=1, long_delay=False):
    palette = bytes([0,0,0, 255,0,0, 0,0,255, 0,255,0, 255,255,255, 0,0,0, 0,0,0, 0,0,0])
    data = bytearray(b"GIF89a" + struct.pack("<HHBBB", 6, 4, 0x82, 0, 0) + palette)
    for _ in range(repeat):
        for x,y,w,h,color,delay,disposal in [(0,0,6,4,1,7,1),(0,0,2,2,2,13,2),(4,0,2,2,3,11,3),(0,2,2,2,4,17,1)]:
            data.extend(b"\x21\xf9\x04" + struct.pack("<BHB", (disposal << 2) | 1, 0 if zero else 2000 if long_delay else delay, 0) + b"\0")
            data.extend(b"\x2c" + struct.pack("<HHHHB", x,y,w,h,0))
            codes = [v for _ in range(w*h) for v in (8,color)] + [9]
            bits = sum(code << (4*i) for i,code in enumerate(codes))
            packed = bits.to_bytes((len(codes)*4+7)//8, "little")
            data.extend(bytes([3,len(packed)]) + packed + b"\0")
    data.append(0x3b)
    path.write_bytes(data)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait(predicate, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(.025)
    raise AssertionError("condition timed out")


def run():
    red, blue, alpha = RUN/"red.png", RUN/"blue.png", RUN/"alpha.png"
    png(red, (255,0,0)); png(blue, (0,0,255)); png(alpha, (255,0,0), 128, True)
    animation, zero, many = RUN/"disposal.gif", RUN/"zero.gif", RUN/"many.gif"
    gif(animation); gif(zero, zero=True); gif(many, repeat=33)
    long_gif, raw_gif = RUN/"long.gif", RUN/"raw-budget.gif"
    gif(long_gif,long_delay=True);gif(raw_gif,repeat=2)
    jpeg = RUN/"red.jpg"
    command(["ffmpeg","-v","error","-threads","1","-i",red,"-frames:v","1","-threads","1",jpeg])
    results = {}
    offsets = [0,69000,70000,199000,200000,309000,310000,479000,480000,1010000]
    phases = [(v % 480000) for v in offsets]
    # Read unencoded pixels from the C harness, not encoder key flags or logs.
    for asset in [red,jpeg,alpha,animation]:
        for ten in (0,1):
            raw = RUN/f"{asset.name}-{ten}.yuv"
            command([BIN/"naki-watermark-render-test", asset, raw, ten])
            data = raw.read_bytes(); stride = 32*24*3//2*(2 if ten else 1)
            assert len(data) == stride*len(offsets)
            def y(frame,x,y):
                pos = frame*stride+(y*32+x)*(2 if ten else 1)
                return int.from_bytes(data[pos:pos+2],"little")/4 if ten else data[pos]
            for i,phase in enumerate(phases):
                assert abs(y(i,28,20)-16) < 2
                if asset == animation:
                    expected = [81,81,81,81]
                    if 70000 <= phase < 200000: expected[0] = 41
                    if phase >= 200000: expected[0] = 16
                    if 200000 <= phase < 310000: expected[1] = 145
                    if phase >= 310000: expected[2] = 235
                    actual = [y(i,x,row) for x,row in [(3,3),(12,3),(3,9),(12,9)]]
                    assert all(abs(a-b)<5 for a,b in zip(actual,expected)), (asset,ten,i,phase,actual,expected)
                elif asset == alpha:
                    assert abs(y(i,3,5)-16)<2 and abs(y(i,12,5)-49)<4, (ten,i,y(i,3,5),y(i,12,5))
                else:
                    assert abs(y(i,8,6)-81)<5, (asset,ten,i,y(i,8,6))
            results[f"pixels-{asset.name}-{ten}"] = "passed"
    for color,expected,black in [("709-limited",63,16),("709-full",54,0)]:
        for ten in (0,1):
            raw=RUN/f"color-{color}-{ten}.yuv"
            command([BIN/"naki-watermark-render-test",red,raw,ten],env=dict(os.environ,NAKI_WM_TEST_COLOR=color))
            data=raw.read_bytes()
            def first_y(x,y):
                pos=(y*32+x)*(2 if ten else 1)
                return int.from_bytes(data[pos:pos+2],"little")/4 if ten else data[pos]
            assert abs(first_y(8,6)-expected)<3 and abs(first_y(28,20)-black)<2,(color,ten,first_y(8,6),first_y(28,20))
            results[f"color-{color}-{ten}"]="passed"
    for asset in [zero,many,long_gif,raw_gif]:
        args=[str(BIN/"naki-watermark-render-test"),str(asset),str(RUN/"reject.yuv"),"0"]
        if asset==raw_gif:args.append("2048")
        p = subprocess.run(args,capture_output=True,timeout=10)
        assert p.returncode == 3, (asset,p.returncode,p.stderr)
        results[f"reject-{asset.name}"] = "passed"
    for mode in ("NAKI_WM_TEST_GATE_UPDATE","NAKI_WM_TEST_OPTIONAL_SETUP","NAKI_WM_TEST_HDR_CHANGE"):
        output=RUN/f"{mode}.yuv"
        command([BIN/"naki-watermark-render-test",red,output,0],env=dict(os.environ,**{mode:"1"}))
        if mode=="NAKI_WM_TEST_GATE_UPDATE":
            data=output.read_bytes();assert abs(data[6*32+8]-81)<5
        if mode=="NAKI_WM_TEST_HDR_CHANGE":assert output.stat().st_size==32*24*3//2
        results[mode]="passed"

    class Assets(http_server.BaseHTTPRequestHandler):
        mutable=red.read_bytes()
        def log_message(self, *args): pass
        def do_GET(self):
            if self.path.startswith("/slow"):
                time.sleep(1.0)
            content = blue.read_bytes() if self.path.startswith("/blue") else red.read_bytes()
            if self.path.startswith("/mutable"):content=Assets.mutable
            if self.path.startswith("/bad"): content = b"not an image"
            if self.path.startswith("/large"):
                self.send_response(200);self.send_header("Content-Length",str(9*1024*1024));self.end_headers()
                try: self.wfile.write(b"x"*(9*1024*1024))
                except (BrokenPipeError,ConnectionResetError): pass
                return
            self.send_response(200);self.send_header("Content-Length",str(len(content)));self.end_headers()
            try: self.wfile.write(content)
            except (BrokenPipeError,ConnectionResetError): pass
    assets = http_server.ThreadingHTTPServer(("127.0.0.1",0),Assets)
    thread = threading.Thread(target=assets.serve_forever,daemon=True);thread.start()
    asset_base = f"http://127.0.0.1:{assets.server_port}"
    began=time.monotonic()
    p=subprocess.run([str(BIN/"naki-watermark-render-test"),asset_base+"/slow",str(RUN/"deadline.yuv"),"0"],
                     env=dict(os.environ,NAKI_WM_TEST_DEADLINE="1"),capture_output=True,timeout=3)
    assert p.returncode==3 and time.monotonic()-began<1,(p.returncode,p.stderr)
    results["startup-budget-not-reset-by-revision"]="passed"
    port = free_port();base = f"http://127.0.0.1:{port}"
    def http(path="/v1/job", data=None, code=200):
        if path in ("/v1/job/stop","/v1/job/watermark") and isinstance(data,dict) and "job_id" not in data:
            data=dict(data,job_id=http()["job_id"])
        req = urllib.request.Request(base+path, data=json.dumps(data).encode() if data is not None else None, headers={"Content-Type":"application/json"})
        try: response=urllib.request.urlopen(req,timeout=4)
        except urllib.error.HTTPError as e: response=e
        with response:
            payload=json.load(response)
            assert response.status==code,(response.status,payload)
            return payload
    def online():
        assert server.poll() is None
        try: return http("/health")
        except OSError: return False
    def config(uri,required=False,**kw):
        c={"enabled":True,"resource":{"uri":str(uri),"load_timeout_ms":3000},
           "startup":{"required":required,"timeout_ms":4000},
           "placement":{"anchor":"top_left","offset":{"x":8,"y":8},"size":{"width":32,"height":32,"fit":"stretch"}}}
        c.update(kw);return c
    def start(name,wm=None,pace=False,encoder="libx264"):
        c={"input":str(sample),"output":str(RUN/f"{name}.mp4"),"output_format":"mp4","pace":pace,"encoder_preset":"ultrafast","frame_log":True}
        if encoder=="libvvenc":c.update(encoder=encoder,encoder_preset="faster")
        if wm is not None:c["watermark"]=wm
        return http(data=c,code=202)
    def terminal(expected="completed"):
        state=wait(lambda: (v if (v:=http())["state"] in ("completed","failed","stopped") else None),20)
        assert state["state"]==expected,state
        return state
    sample=RUN/"input.mp4"
    command(["ffmpeg","-v","error","-f","lavfi","-i","color=black:size=160x96:rate=25:duration=5","-c:v","libx264","-preset","ultrafast","-threads","2",sample])
    log=(RUN/"server.jsonl").open("wb");err=(RUN/"server.stderr").open("wb")
    state_directory=tempfile.mkdtemp(prefix="wm-",dir=os.environ.get("TMPDIR"))
    def serve():
        return subprocess.Popen([str(BIN/"naki-live"),"--port",str(port),"--run-dir",str(RUN/"logs"),"--state-dir",state_directory,"--status-interval","1"],stdout=log,stderr=err,cwd=ROOT)
    server=serve()
    try:
        health=wait(online)
        assert health["watermark_api"]==1 and health["watermark_codecs"]==1,health
        # Required resource delay is inside startup, before input open or output.
        start("required",config(asset_base+"/slow",True),True)
        state=wait(lambda: (v if (v:=http())["state"]=="waiting_assets" else None))
        assert state["packets_out"]==0 and not (RUN/"required.mp4").exists(),state
        state=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==1 else None))
        assert state["startup"]["request_to_first_video_mux_ms"]>=900,state
        http("/v1/job/watermark",{"revision":2,"watermark":{"enabled":False}},409)
        update=http("/v1/job/watermark",{"revision":2,"watermark":config(blue,True)},202)
        assert update["accepted"]==1
        wait(lambda: http().get("watermark",{}).get("applied_revision")==2)
        http("/v1/job/watermark",{"revision":2,"watermark":config(red,True)},409)
        # Failed candidate keeps active pixels and revision.
        http("/v1/job/watermark",{"revision":3,"watermark":config(asset_base+"/bad",True)},202)
        state=wait(lambda: (v if (v:=http()).get("watermark",{}).get("failed_revision")==3 else None))
        assert state["watermark"]["applied_revision"]==2,state
        state=terminal();results["required-hot-update-failure"] = state
        pixels=command(["ffmpeg","-v","error","-i",RUN/"required.mp4","-vf","crop=2:2:20:20","-pix_fmt","rgb24","-f","rawvideo","-"])
        colors=[pixels[i:i+3] for i in range(0,len(pixels),12)]
        assert colors[0][0]>180 and colors[0][2]<60, colors[0]
        assert any(c[2]>180 and c[0]<60 for c in colors), colors[-1]
        # Anchor/offset and size are real frame changes; a repeated identical
        # URI/configuration must fetch changed content, rather than pinning URI.
        c=config(asset_base+"/mutable",True);start("layout",c,True)
        wait(lambda: http().get("watermark",{}).get("applied_revision")==1)
        c["placement"]["anchor"]="bottom_right";c["placement"]["offset"]={"x":-8,"y":-8}
        http("/v1/job/watermark",{"revision":2,"watermark":c},202)
        second=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==2 else None))
        c["placement"]["size"]={"width":16,"height":16,"fit":"stretch"}
        http("/v1/job/watermark",{"revision":3,"watermark":c},202)
        third=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==3 else None))
        Assets.mutable=blue.read_bytes()
        http("/v1/job/watermark",{"revision":4,"watermark":c},202)
        fourth=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==4 else None))
        assert fourth["watermark"]["content_sha256"]==hashlib.sha256(blue.read_bytes()).hexdigest()
        state=terminal();assert state["frames_filtered"]==125 and state["packets_out"]==125,state
        pixels=command(["ffmpeg","-v","error","-i",RUN/"layout.mp4","-pix_fmt","rgb24","-f","rawvideo","-"])
        assert len(pixels)==125*160*96*3
        def pixel(index,x,y):
            pos=(index*160*96+y*160+x)*3;return pixels[pos:pos+3]
        def red_pixel(p):return p[0]>180 and p[1]<60 and p[2]<60
        def black_pixel(p):return max(p)<30
        i2=second["watermark"]["applied_source_pts_us"]//40000
        i3=third["watermark"]["applied_source_pts_us"]//40000
        i4=fourth["watermark"]["applied_source_pts_us"]//40000
        assert red_pixel(pixel(0,24,24)) and black_pixel(pixel(0,128,64))
        assert black_pixel(pixel(i2,24,24)) and red_pixel(pixel(i2,128,64)),(i2,pixel(i2,24,24),pixel(i2,128,64))
        assert black_pixel(pixel(i3,128,64)) and red_pixel(pixel(i3,144,80)),(i3,pixel(i3,128,64),pixel(i3,144,80))
        assert pixel(i4,144,80)[2]>180 and pixel(i4,144,80)[0]<60,(i4,pixel(i4,144,80))
        results["layout-and-same-uri-refresh"]={"second":second["watermark"],"third":third["watermark"],"fourth":fourth["watermark"],"status":state}
        # No watermark -> pending optional -> latest revision, disable, re-enable.
        start("optional",pace=True)
        wait(lambda: http().get("watermark",{}).get("applied_revision")==1)
        pid=http()["supervisor"]["worker_pid"]
        http("/v1/job/watermark",{"revision":2,"watermark":config(asset_base+"/slow")},202)
        http("/v1/job/watermark",{"revision":3,"watermark":config(animation)},202)
        wait(lambda: http().get("watermark",{}).get("applied_revision")==3)
        http("/v1/job/watermark",{"revision":4,"watermark":{"enabled":False}},202)
        wait(lambda: http().get("watermark",{}).get("applied_revision")==4)
        # Controller death must not cancel media or lose the accepted revision.
        server.kill();server.wait(timeout=3);server=serve();wait(online)
        wait(lambda: http().get("supervisor",{}).get("worker_pid")==pid)
        http("/v1/job/watermark",{"revision":5,"watermark":config(red)},202)
        state=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==5 else None))
        assert state["supervisor"]["worker_pid"]==pid
        for revision in range(6,12):
            c=config(blue);c["placement"]["size"]={"width":16+revision*4,"height":16+revision*2}
            http("/v1/job/watermark",{"revision":revision,"watermark":c},202)
        state=wait(lambda: (v if (v:=http()).get("watermark",{}).get("applied_revision")==11 else None))
        assert state["supervisor"]["worker_pid"]==pid and state["watermark"]["active_raw_bytes"]<=64*1024*1024
        http("/v1/job/stop",{},202);results["optional-coalesce-disable-recover"] = terminal("stopped")
        # The integrated 10-bit/B-picture encoder path must preserve all frame
        # identities and watermark revisions, beyond the unencoded pixel test.
        start("vvc-watermark",config(animation,True),encoder="libvvenc")
        state=terminal();assert state["frames_filtered"]==125 and state["packets_out"]==125,state
        cache=pathlib.Path(os.environ.get("NAKI_BUILD_ROOT",pathlib.Path.home()/".cache/naki-live-transcode-n9"))
        probe=json.loads(command([cache/"install/bin/ffprobe","-v","error","-select_streams","v:0","-count_frames",
            "-show_entries","stream=pix_fmt,nb_read_frames","-of","json",RUN/"vvc-watermark.mp4"]))["streams"][0]
        assert probe["pix_fmt"]=="yuv420p10le" and int(probe["nb_read_frames"])==125,probe
        prefix=pathlib.Path(state["log_path"])
        wait(lambda: not list(prefix.parent.glob(prefix.name+".*.active")))
        traces=[json.loads(line) for file in prefix.parent.glob(prefix.name+".*.jsonl") for line in file.read_text().splitlines()]
        frames=[row for row in traces if row.get("event")=="frame"]
        assert len(frames)==125 and all(row["watermark_revision"]==1 for row in frames)
        assert sorted(row["frame_id"] for row in frames)==list(range(1,126))
        results["vvc-10bit-watermark-frame-ledger"]={"status":state,"probe":probe}
        # Required bad/missing image cannot create an output header.
        for name,uri in [("invalid",asset_base+"/bad"),("missing",RUN/"missing.png"),("oversize",asset_base+"/large"),("gif-budget",many)]:
            start(name,config(uri,True));state=terminal("failed")
            assert state["packets_out"]==0 and not (RUN/f"{name}.mp4").exists(),state
            results[name]=state
        c=config(asset_base+"/slow",True);c["startup"]["timeout_ms"]=100
        start("timeout",c);state=terminal("failed")
        assert state["packets_out"]==0 and not (RUN/"timeout.mp4").exists()
        results["startup-timeout"]=state
        start("cancel",config(asset_base+"/slow",True));http("/v1/job/stop",{},202)
        results["startup-cancel"]=terminal("stopped")
    finally:
        if server.poll() is None:server.send_signal(signal.SIGTERM);server.wait(timeout=5)
        assets.shutdown();assets.server_close();log.close();err.close()
    (RUN/"COMPLETE.json").write_text(json.dumps({"status":"passed","cases":results,"gif_sha256":hashlib.sha256(animation.read_bytes()).hexdigest()},indent=2))
    print(f"Watermark checks passed: {RUN}")


if __name__=="__main__":run()
