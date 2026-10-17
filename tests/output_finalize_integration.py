#!/usr/bin/env python3
"""A peer resets only after the real trailer, so protocol close is the first failure."""
import json
import os
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time

from control_lifecycle_integration import ROOT, BIN, RUN, Server, wait


class Receiver:
    def __init__(self):
        self.listener=socket.socket();self.listener.bind(("127.0.0.1",0));self.listener.listen(1)
        self.listener.settimeout(8);self.url=f"http://127.0.0.1:{self.listener.getsockname()[1]}/upload"
        self.close_now=threading.Event();self.closed=threading.Event();self.bytes=0;self.error=None
        self.thread=threading.Thread(target=self.read,daemon=True);self.thread.start()

    def read(self):
        try:
            connection,_=self.listener.accept()
            with connection:
                connection.settimeout(.05);header=b""
                while b"\r\n\r\n" not in header:header+=connection.recv(4096)
                assert b"Transfer-Encoding: chunked" in header,header
                connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
                while not self.close_now.is_set():
                    try:
                        data=connection.recv(65536)
                        if not data:break
                        self.bytes+=len(data)
                    except socket.timeout:pass
                connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack("ii",1,0))
            self.closed.set()
        except BaseException as error:self.error=repr(error);self.closed.set()

    def close(self):
        self.close_now.set();self.listener.close();self.thread.join(timeout=3)
        assert not self.thread.is_alive() and not self.error,self.error


def main():
    evidence=RUN/"finalize";evidence.mkdir()
    binaries=evidence/"bin";binaries.mkdir()
    for name in ("naki-live","naki-crash-collector"):shutil.copy2(BIN/name,binaries/name)
    shutil.copy2(BIN/"naki-live-hook-worker",binaries/"naki-live-worker")
    sample=evidence/"input.mp4"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=160x90:rate=25",
                    "-frames:v","5","-c:v","libx264","-threads","2",str(sample)],check=True,timeout=20)
    results={}
    for multi in (False,True):
        name="multi" if multi else "single";case=evidence/name;case.mkdir()
        ready=case/"trailer-finished";release=case/"close-release"
        environment=dict(LD_PRELOAD=str(BIN/"libnaki-output-finalize-barrier.so"),NAKI_TEST_FINALIZE_READY=str(ready),
                         NAKI_TEST_FINALIZE_RELEASE=str(release),NAKI_TEST_MEDIA_HOOK_DIR=str(case),NAKI_TEST_HOOK_OUTPUT_PATHS="1")
        receiver=Receiver();server=Server("finalize-"+name,binaries/"naki-live",environment=environment)
        try:
            spec=dict(input=str(sample),output=receiver.url,output_format="mpegts",encoder_preset="ultrafast")
            if multi:
                spec={"input":str(sample),"outputs":[dict(id="remote",output=receiver.url,output_format="mpegts",encoder_preset="ultrafast"),
                                                       dict(id="healthy",output=str(case/"healthy.mp4"),output_format="mp4",encoder_preset="ultrafast")]}
            accepted=server.api(body=spec,code=202)
            wait(lambda:ready.exists())
            assert receiver.bytes>0
            receiver.close_now.set();assert receiver.closed.wait(2) and not receiver.error,receiver.error
            time.sleep(.05);began=time.monotonic();release.touch()
            terminal=wait(lambda:(s if (s:=server.api())["state"] in ("completed","completed_partial","failed") else None))
            assert time.monotonic()-began<5,terminal
            assert terminal["state"]==("completed_partial" if multi else "failed"),terminal
            target=next(o for o in terminal["outputs"] if o["output_id"]=="remote") if multi else terminal
            final=target["output_finalize"]
            assert final["trailer_error"]==final["flush_error"]==0 and final["close_error"]<0,final
            assert final["primary_error"]==final["close_error"] and final["finished_us"]>=final["started_us"],final
            suffix="remote" if multi else "main"
            hook=json.loads((case/f'{accepted["job_id"]}-{suffix}.json').read_text())
            assert hook["error_code"]==final["close_error"] and hook["cancelled"]==0,hook
            if multi:
                healthy=next(o for o in terminal["outputs"] if o["output_id"]=="healthy")
                assert healthy["state"]=="completed" and healthy["packets_out"]==5,healthy
            (case/"terminal.json").write_text(json.dumps(terminal,indent=2)+"\n")
            results[name]="passed"
        finally:
            release.touch();server.close();receiver.close()
    (evidence/"results.json").write_text(json.dumps(results,indent=2)+"\n")
    print(json.dumps(dict(evidence=str(evidence),results=results),indent=2))


if __name__=="__main__":main()
