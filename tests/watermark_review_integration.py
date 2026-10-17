#!/usr/bin/env python3
"""Independent-review regressions: first-video races, color, control and TLS."""
import binascii
import http.server
import json
import os
import pathlib
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib

ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get("NAKI_SERVICE_BIN",ROOT/"out/bin"))
RUN=pathlib.Path(tempfile.mkdtemp(prefix="watermark-review-",dir=ROOT/"out"))


def command(args,timeout=30):
    p=subprocess.run(list(map(str,args)),capture_output=True,timeout=timeout)
    assert p.returncode==0,(args,p.returncode,p.stderr[-3000:])
    return p.stdout


def wait(predicate,timeout=10):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=predicate()
        if result:return result
        time.sleep(.02)
    raise AssertionError("condition timed out")


def png(path,primaries=None):
    def chunk(kind,data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",binascii.crc32(kind+data)&0xffffffff)
    data=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",8,8,8,6,0,0,0))
    if primaries:data+=chunk(b"cHRM",struct.pack(">8I",*primaries))
    data+=chunk(b"IDAT",zlib.compress((b"\0"+bytes([255,0,0,255])*8)*8))+chunk(b"IEND",b"")
    path.write_bytes(data)


class GatedInput:
    """Accept TCP now; deliver video and EOF only at explicit test checkpoints."""
    def __init__(self,data):
        self.data=data;self.connected=threading.Event();self.release=threading.Event();self.finish=threading.Event()
        self.listener=socket.socket();self.listener.bind(("127.0.0.1",0));self.listener.listen(1);self.listener.settimeout(8)
        self.url=f"tcp://127.0.0.1:{self.listener.getsockname()[1]}"
        self.thread=threading.Thread(target=self.send,daemon=True)
    def send(self):
        try:
            conn,_=self.listener.accept()
            with conn:
                self.connected.set()
                if self.release.wait(8):
                    conn.sendall(self.data)
                    self.finish.wait(8)
                    conn.shutdown(socket.SHUT_WR)
        except (OSError,TimeoutError):pass
    def __enter__(self):self.thread.start();return self
    def __exit__(self,*_):
        self.release.set();self.finish.set()
        try:self.listener.shutdown(socket.SHUT_RDWR)
        except OSError:pass
        self.listener.close();self.thread.join(timeout=3)
        assert not self.thread.is_alive()


def run():
    results={};red=RUN/"red.png";png(red)
    results["operation-owner-races"]=command([BIN/"naki-watermark-operation-test",red]).decode().strip()
    command([BIN/"naki-watermark-state-test",red,RUN/"boundary-state.json"])
    matrix=json.loads((RUN/"boundary-state.json").read_text())
    assert matrix["count"]==33 and matrix["failures"]==0,matrix
    results["boundary-state-matrix"]=matrix
    bt709=RUN/"bt709.png";rounded=RUN/"rounded-bt709.png";bt2020=RUN/"bt2020.png";unknown=RUN/"unknown-chrm.png"
    png(bt709,[31270,32900,64000,33000,30000,60000,15000,6000])
    png(rounded,[31271,32900,64000,33000,30000,60000,15000,6000])
    png(bt2020,[31270,32900,70800,29200,17000,79700,13100,4600])
    png(unknown,[31270,32900,65000,34000,28000,59000,16000,7000])
    def render(name,uri,expected=0,extra_env=None):
        args=[str(BIN/"naki-watermark-render-test"),str(uri),str(RUN/f"{name}.yuv"),"0"]
        env=dict(os.environ);env.update(extra_env or {})
        began=time.monotonic();p=subprocess.run(args,capture_output=True,env=env,timeout=10)
        (RUN/f"{name}.log").write_bytes(p.stdout+p.stderr)
        assert p.returncode==expected,(name,p.returncode,p.stderr[-2000:])
        results[name]={"exit":p.returncode,"elapsed_s":time.monotonic()-began}
    render("bt709-chrm",bt709)
    render("rounded-bt709-chrm",rounded)
    render("bt2020-chrm-rejected",bt2020,3)
    render("unknown-chrm-rejected",unknown,3)

    class Assets(http.server.BaseHTTPRequestHandler):
        slow_started=threading.Event();slow_release=threading.Event()
        hits=[];tls_url=""
        def log_message(self,*_):pass
        def do_GET(self):
            self.hits.append((self.server.server_port,self.path))
            if self.path.startswith("/redirect/"):
                _,_,count,kind=self.path.split("/")
                if kind=="slow":time.sleep(.08)
                if int(count):
                    self.send_response(302);self.send_header("Location",f"/redirect/{int(count)-1}/{kind}");self.end_headers();return
            if self.path=="/loop":
                self.send_response(302);self.send_header("Location","/loop");self.end_headers();return
            if self.path=="/file-redirect":
                self.send_response(302);self.send_header("Location",red.as_uri());self.end_headers();return
            if self.path=="/https-redirect":
                self.send_response(302);self.send_header("Location",self.tls_url);self.end_headers();return
            if self.path=="/held":
                self.slow_started.set();self.slow_release.wait(5)
            data=red.read_bytes();self.send_response(200);self.send_header("Content-Length",str(len(data)));self.end_headers()
            try:self.wfile.write(data)
            except (BrokenPipeError,ConnectionResetError):pass
    assets=http.server.ThreadingHTTPServer(("127.0.0.1",0),Assets)
    threading.Thread(target=assets.serve_forever,daemon=True).start()
    asset_base=f"http://127.0.0.1:{assets.server_port}"
    tls=None
    try:
        render("redirect-three-allowed",asset_base+"/redirect/3/image")
        render("redirect-four-rejected",asset_base+"/redirect/4/image",3)
        render("redirect-loop-rejected",asset_base+"/loop",3)
        render("redirect-file-protocol-rejected",asset_base+"/file-redirect",3)
        render("redirect-one-deadline",asset_base+"/redirect/3/slow",3,{"NAKI_WM_TEST_LOAD_TIMEOUT_MS":"150"})
        assert results["redirect-one-deadline"]["elapsed_s"]<1
        # Existing OpenSSL only; never add a root certificate or disable client verification.
        if shutil.which("openssl"):
            cert=RUN/"selfsigned.pem";key=RUN/"selfsigned.key"
            command(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-sha256","-days","1",
                     "-subj","/CN=localhost","-addext","subjectAltName=IP:127.0.0.1,DNS:localhost","-keyout",key,"-out",cert])
            tls=http.server.ThreadingHTTPServer(("127.0.0.1",0),Assets)
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
            tls.socket=context.wrap_socket(tls.socket,server_side=True)
            threading.Thread(target=tls.serve_forever,daemon=True).start()
            Assets.tls_url=f"https://127.0.0.1:{tls.server_port}/image"
            render("https-untrusted-certificate-rejected",Assets.tls_url,3)
            render("redirect-keeps-tls-verification",asset_base+"/https-redirect",3)
            assert not any(port==tls.server_port for port,_ in Assets.hits),Assets.hits
        else:results["https-untrusted-certificate-rejected"]={"status":"not_run","reason":"OpenSSL command not already available"}
        trusted_url=os.environ.get("NAKI_TEST_HTTPS_URL")
        if trusted_url:render("https-trusted-default-verification",trusted_url)
        else:results["https-trusted-default-verification"]={"status":"not_run","reason":"no configured trusted HTTPS fixture"}

        def source(name,hdr=False,count=5):
            path=RUN/f"{name}.ts"
            args=["ffmpeg","-v","error","-f","lavfi","-i","color=black:size=32x24:rate=25","-frames:v",count,
                  "-c:v","libx264","-preset","ultrafast","-threads","2","-g","1","-bf","0"]
            if hdr:args += ["-color_trc","smpte2084","-color_primaries","bt2020","-colorspace","bt2020nc"]
            command([*args,"-muxdelay","0","-muxpreload","0","-f","mpegts",path]);return path.read_bytes()
        plain=source("plain");hdr=source("hdr",True)
        listener=socket.socket();listener.bind(("127.0.0.1",0));port=listener.getsockname()[1];listener.close()
        base=f"http://127.0.0.1:{port}"
        def api(path="/v1/job",body=None,code=200):
            if path in ("/v1/job/stop","/v1/job/watermark") and isinstance(body,dict) and "job_id" not in body:
                body=dict(body,job_id=api()["job_id"])
            req=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers={"Content-Type":"application/json"})
            try:response=urllib.request.urlopen(req,timeout=4)
            except urllib.error.HTTPError as error:response=error
            with response:
                value=json.load(response)
                assert response.status in (code if isinstance(code,tuple) else (code,)),(path,response.status,value)
                return value
        def config(uri):
            return {"enabled":True,"resource":{"uri":str(uri)},"placement":{"anchor":"top_left","size":{"width":16,"height":16}}}
        def done(expected):
            state=wait(lambda:(s if (s:=api())["state"] in ("completed","failed","stopped") else None))
            assert state["state"]==expected,state;return state
        log=(RUN/"server.jsonl").open("wb");err=(RUN/"server.stderr").open("wb")
        server=subprocess.Popen([str(BIN/"naki-live"),"--port",str(port),"--run-dir",str(RUN/"logs"),
                                 "--state-dir",tempfile.mkdtemp(prefix="wmr-",dir=os.environ.get("TMPDIR"))],stdout=log,stderr=err,cwd=ROOT)
        try:
            def online():
                assert server.poll() is None
                try:return api("/health")
                except OSError:return False
            wait(online)
            # Actual HTTP update arrives after input connection, before any video bytes.
            for name,data,large,initial,held in [
                ("hdr-prepared-update",hdr,False,False,False),("hdr-loading-update",hdr,False,False,True),
                ("large-prepared-update",plain,True,False,False),
                ("hdr-initial-enabled",hdr,False,True,False),("large-initial-enabled",plain,True,True,False)]:
                if large:
                    job={"input":"udp://127.0.0.1:1","output":str(RUN/f"{name}.mp4"),"output_format":"mp4","width":4096,"height":2162}
                    if initial:job["watermark"]=config(red)
                    rejected=api(body=job,code=400);assert "live-4k-v1" in rejected["error"],rejected
                    results[name]={"stage":"static-resource-profile","http_status":400};continue
                with GatedInput(data) as incoming:
                    output=RUN/f"{name}.mp4"
                    job={"input":incoming.url,"output":str(output),"output_format":"mp4","encoder_preset":"ultrafast"}
                    if large:job.update(width=4096,height=2162)
                    if initial:job["watermark"]=config(red)
                    accepted=api(body=job,code=202);assert incoming.connected.wait(5)
                    if not initial:
                        wm=wait(lambda:api().get("watermark"));assert not wm["video_properties_known"]
                        Assets.slow_started.clear();Assets.slow_release.clear()
                        uri=asset_base+"/held" if held else red
                        api("/v1/job/watermark",{"revision":2,"watermark":config(uri)},202)
                        if held:assert Assets.slow_started.wait(3)
                        else:wait(lambda:api().get("watermark",{}).get("prepared_revision")==2)
                        assert not api()["watermark"]["video_properties_known"]
                    incoming.release.set()
                    if not initial:
                        state=wait(lambda:(s if (s:=api()).get("watermark",{}).get("video_properties_known") else None))
                        assert state["watermark"]["failed_revision"]==2 and state["watermark"]["active_frames"]==0,state
                        assert state["supervisor"]["worker_pid"]==accepted["supervisor"]["worker_pid"]
                        Assets.slow_release.set()
                        api("/v1/job/watermark",{"revision":3,"watermark":config(red)},409)
                    incoming.finish.set();state=done("failed" if initial else "completed")
                    if initial:assert not output.exists() and state["packets_out"]==0,state
                    else:
                        assert state["packets_out"]==5 and state["watermark"]["failed_revision"]==2,state
                        pixels=command(["ffmpeg","-v","error","-i",output,"-vf","crop=2:2:2:2","-pix_fmt","rgb24","-f","rawvideo","-"])
                        assert len(pixels)==5*12 and max(pixels)<30,(name,pixels)
                    results[name]=state
            # A prepared close supersedes initial enable at the first boundary,
            # including when the obsolete initial asset is still being fetched.
            for name,data,large,held in [
                ("hdr-close-before-first",hdr,False,False),("large-close-before-first",plain,True,False),
                ("hdr-close-during-load",hdr,False,True),("large-close-during-load",plain,True,True)]:
                if large:
                    rejected=api(body={"input":"udp://127.0.0.1:1","output":str(RUN/f"{name}.mp4"),"output_format":"mp4", "width":4096,"height":2162,"watermark":config(red)},code=400)
                    assert "live-4k-v1" in rejected["error"],rejected
                    results[name]={"stage":"static-resource-profile","http_status":400};continue
                with GatedInput(data) as incoming:
                    Assets.slow_started.clear();Assets.slow_release.clear()
                    output=RUN/f"{name}.mp4"
                    job={"input":incoming.url,"output":str(output),"output_format":"mp4","encoder_preset":"ultrafast",
                         "watermark":config(asset_base+"/held" if held else red)}
                    if large:job.update(width=4096,height=2162)
                    accepted=api(body=job,code=202);assert incoming.connected.wait(5)
                    if held:assert Assets.slow_started.wait(3)
                    else:wait(lambda:api().get("watermark",{}).get("prepared_revision")==1)
                    api("/v1/job/watermark",{"revision":2,"watermark":{"enabled":False}},202)
                    wait(lambda:api().get("watermark",{}).get("prepared_revision")==2)
                    assert not api()["watermark"]["video_properties_known"]
                    incoming.release.set()
                    state=wait(lambda:(s if (s:=api()).get("watermark",{}).get("video_properties_known") else None))
                    assert state["supervisor"]["worker_pid"]==accepted["supervisor"]["worker_pid"]
                    Assets.slow_release.set();incoming.finish.set();state=done("completed")
                    assert state["packets_out"]==5 and state["watermark"]["applied_revision"]==2,state
                    assert state["watermark"]["failed_revision"]!=2,state
                    pixels=command(["ffmpeg","-v","error","-i",output,"-vf","crop=2:2:2:2","-pix_fmt","rgb24","-f","rawvideo","-"])
                    assert len(pixels)==5*12 and max(pixels)<30,(name,pixels)
                    results[name]=state
            # Existing active image survives a legal cHRM request with unsupported primaries.
            sample=RUN/"paced.mp4"
            command(["ffmpeg","-v","error","-f","lavfi","-i","color=black:size=160x96:rate=25:duration=5",
                     "-c:v","libx264","-preset","ultrafast","-threads","2",sample])
            api(body={"input":str(sample),"output":str(RUN/"chrm-hot.mp4"),"output_format":"mp4","pace":True,
                      "encoder_preset":"ultrafast","watermark":dict(config(red),startup={"required":True})},code=202)
            wait(lambda:api().get("watermark",{}).get("applied_revision")==1)
            cfg=dict(config(bt2020),startup={"required":True})
            api("/v1/job/watermark",{"revision":2,"watermark":cfg},202)
            state=wait(lambda:(s if (s:=api()).get("watermark",{}).get("failed_revision")==2 else None))
            assert state["watermark"]["applied_revision"]==1 and state["watermark"]["content_sha256"],state
            api("/v1/job/stop",{},202);state=done("stopped")
            pixels=command(["ffmpeg","-v","error","-i",RUN/"chrm-hot.mp4","-vf","crop=2:2:6:6","-pix_fmt","rgb24","-f","rawvideo","-"])
            assert pixels and all(pixels[i]>180 and pixels[i+2]<60 for i in range(0,len(pixels),3))
            results["bt2020-hot-keeps-old-pixels"]=state
            # Pause our verified worker via pidfd so the client can disconnect
            # while the controller owns an HTTP request, before ACK or timeout.
            accepted=api(body={"input":str(sample),"output":str(RUN/"ack-disconnect.mp4"),"output_format":"mp4",
                               "pace":True,"encoder_preset":"ultrafast"},code=202)
            wait(lambda:api().get("packets_out",0)>0);pid=accepted["supervisor"]["worker_pid"]
            fd=os.pidfd_open(pid)
            try:
                status=pathlib.Path(f"/proc/{pid}/status").read_text()
                assert f"PPid:\t{server.pid}\n" in status
                for revision,timeout in [(2,False),(3,True)]:
                    retry=None;retry_result=[]
                    signal.pidfd_send_signal(fd,signal.SIGSTOP)
                    try:
                        payload=json.dumps({"job_id":accepted["job_id"],"revision":revision,"watermark":config(red)}).encode()
                        raw=socket.create_connection(("127.0.0.1",port))
                        raw.sendall(b"POST /v1/job/watermark HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: "+str(len(payload)).encode()+b"\r\n\r\n"+payload)
                        wait(lambda:api("/v1/job/watermark",{"invalid":True},(400,409)).get("error")=="Watermark acknowledgement pending",1)
                        raw.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack("ii",1,0));raw.close()
                        if timeout:
                            time.sleep(2.2)
                            api("/v1/job/watermark",{"invalid":True},400)
                            # Reuse the timed-out revision while its old ACK is
                            # still delayed. The old 202 must not answer this
                            # new request, which the worker must reject as 409.
                            def retry_same_revision():
                                try:retry_result.append(api("/v1/job/watermark",{"revision":revision,"watermark":config(red)},409))
                                except Exception as error:retry_result.append(error)
                            retry=threading.Thread(target=retry_same_revision,daemon=True);retry.start()
                            wait(lambda:api("/v1/job/watermark",{"invalid":True},(400,409)).get("error")=="Watermark acknowledgement pending",1)
                    finally:signal.pidfd_send_signal(fd,signal.SIGCONT)
                    if retry:
                        retry.join(timeout=4)
                        assert not retry.is_alive() and len(retry_result)==1 and isinstance(retry_result[0],dict),retry_result
                        assert retry_result[0]["accepted"]==0,retry_result
                    wait(lambda:api().get("watermark",{}).get("applied_revision")==revision)
                    assert api("/health")["status"]=="ok"
                api("/v1/job/watermark",{"revision":4,"watermark":{"enabled":False}},202)
                wait(lambda:api().get("watermark",{}).get("applied_revision")==4)
                api("/v1/job/stop",{},202);results["ack-disconnect-and-timeout"]=done("stopped")
            finally:os.close(fd)
        finally:
            if server.poll() is None:server.send_signal(signal.SIGTERM);server.wait(timeout=5)
            log.close();err.close()
    finally:
        Assets.slow_release.set();assets.shutdown();assets.server_close()
        if tls:tls.shutdown();tls.server_close()
    (RUN/"COMPLETE.json").write_text(json.dumps({"status":"passed","cases":results},indent=2)+"\n")
    print(f"Watermark review checks passed: {RUN}")


if __name__=="__main__":run()
