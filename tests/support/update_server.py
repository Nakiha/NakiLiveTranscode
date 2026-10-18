"""Explicit per-run service launcher; no fixtures are created on import."""
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from .service_harness import ROOT, BIN, CACHE, port

def wait(predicate,timeout=10):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        result=predicate()
        if result:return result
        time.sleep(.015)
    raise AssertionError('condition timed out')

def scoped_signal(pid,sig):
    fd=os.pidfd_open(pid)
    try:signal.pidfd_send_signal(fd,sig)
    finally:os.close(fd)

class Server:
    def __init__(self, run_dir,name,binary=None,environment=None):
        with socket.socket() as s:s.bind(('127.0.0.1',0));self.port=s.getsockname()[1]
        self.run_dir=run_dir;self.name=name;self.base=f'http://127.0.0.1:{self.port}'
        self.state=tempfile.TemporaryDirectory(prefix='upd-')
        self.logs=self.run_dir/name;self.logs.mkdir();self.binary=binary or BIN/'naki-live'
        self.environment=dict(os.environ,**(environment or {}));self.outputs=[];self.start()
    def start(self):
        index=len(self.outputs);log=(self.logs/f'controller-{index}.stdout').open('wb');error=(self.logs/f'controller-{index}.stderr').open('wb');self.outputs.extend([log,error])
        self.process=subprocess.Popen([str(self.binary),'--port',str(self.port),'--run-dir',str(self.logs/'logs'),
            '--state-dir',self.state.name,'--worker-stop-timeout','3','--worker-timeout','5','--spare-workers','1'],stdout=log,stderr=error,env=self.environment)
        def online():
            assert self.process.poll() is None,(self.name,self.process.returncode)
            try:return self.api('/health')
            except OSError:return None
        wait(online)
    def api(self,path='/v1/job',body=None,code=200,request_id=None):
        headers={'Content-Type':'application/json'}
        if request_id:headers['X-Request-ID']=request_id
        req=urllib.request.Request(self.base+path,data=json.dumps(body).encode() if body is not None else None,headers=headers)
        try:r=urllib.request.urlopen(req,timeout=4)
        except urllib.error.HTTPError as e:r=e
        with r:
            result=json.load(r);assert r.status in (code if isinstance(code,tuple) else (code,)),(path,r.status,result);return result
    def restart(self):
        self.process.kill();self.process.wait(timeout=3);self.start()
        wait(lambda:not self.api('/health')['pool']['recovering'])
    def close(self):
        if self.process.poll() is None:self.process.terminate();self.process.wait(timeout=8)
        for f in self.outputs:f.close()
        self.state.cleanup()
