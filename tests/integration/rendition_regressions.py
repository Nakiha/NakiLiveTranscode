#!/usr/bin/env python3
"""Audit regressions: real migrated sockets, required resources and terminal I/O."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import json
import os
import signal
import socket
import tempfile
import threading
import time
import uuid
from integration.rendition_process_integration import ROOT, BIN, Harness, command, wait, require, decoded_hashes
from support.images import png


class Sink:
    def __init__(self):
        self.socket=socket.socket();self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
        self.socket.bind(('127.0.0.1',0));self.socket.listen();self.socket.settimeout(.1)
        self.port=self.socket.getsockname()[1];self.stop=threading.Event();self.stall=threading.Event()
        self.connected=threading.Event();self.closed=threading.Event();self.bytes=0;self.duration=None;self.error=None;self.connection=None
        self.thread=threading.Thread(target=self.read,daemon=True);self.thread.start()
    def read(self):
        try:
            while not self.stop.is_set():
                try:self.connection,_=self.socket.accept();break
                except socket.timeout:continue
            else:return
            began=time.monotonic();self.connection.settimeout(.1);self.connected.set()
            while not self.stop.is_set():
                if self.stall.is_set():self.stop.wait(.02);continue
                try:
                    data=self.connection.recv(65536)
                    if not data:self.duration=time.monotonic()-began;self.closed.set();return
                    self.bytes+=len(data)
                except socket.timeout:pass
        except OSError as error:
            if not self.stop.is_set():self.error=repr(error)
    def close(self):
        self.stop.set();self.socket.close()
        if self.connection:self.connection.close()
        self.thread.join(2);require(not self.thread.is_alive() and not self.error,self.error)


def output(directory,name='main',**overrides):
    return dict(dict(id=name,output=str(directory/(name+'.mp4')),output_format='mp4',width=320,height=192,encoder_preset='ultrafast',audio_copy=False),**overrides)


def config(sample,outputs,backend):
    return dict(input=str(sample),outputs=outputs,rendition_backend=backend,pace=True,max_threads=2,min_threads=2,
                decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'})


def topology(h,action,name='main',destination=None,spec=None,expected=202):
    state=h.api();body=dict(operation_id=uuid.uuid4().hex,action=action,expected_revision=state['topology_revision'],target=dict(component='destination' if destination else 'rendition',output_id=name))
    if action!='add_rendition':body['expected_generation']=next(x['rendition_generation'] for x in state['outputs'] if x['output_id']==name)
    if destination:body['target']['destination_id']=destination
    if spec is not None:body['spec']=spec
    h.api(f'/v1/jobs/{state["job_id"]}/updates',body,expected)
    return state['job_id'],body


def operation(h,job,body,delivery=True):
    def poll():
        result=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}')
        require(result['state'] not in ('failed','cancelled'),result)
        return result if result['state']=='applied' and (not delivery or result['delivery']['state']=='first_written') else None
    return wait(poll,12)


def terminal(h):
    return wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)


def migrated_primary(directory,sample,backend,frames):
    directory.mkdir();h=Harness(directory);sink=Sink()
    try:
        h.start();primary=output(directory,output=f'tcp://127.0.0.1:{sink.port}?send_buffer_size=4096&tcp_nodelay=1',output_format='flv')
        h.api(body=config(sample,[primary],backend),expected=202);wait(lambda:h.api().get('packets_out',0)>=8)
        # Neither protocol aliases nor another spelling of the primary may open
        # a file outside the exclusive-create route.
        victim=directory/'existing.mp4';sentinel=b'audit sentinel'*1024;victim.write_bytes(sentinel)
        for uri,fmt in [('file:'+str(victim),'mp4'),('rtmp://127.0.0.1/live','mp4')]:
            topology(h,'add_destination',destination='invalid',spec=dict(output=uri,output_format=fmt),expected=400)
        require(victim.read_bytes()==sentinel,'rejected URI changed existing bytes')
        job,body=topology(h,'add_destination',destination='mirror',spec=dict(output=str(directory/'mirror.mp4'),output_format='mp4'))
        joined=operation(h,job,body);pid=h.api()['outputs'][0]['rendition_pid'];sink.stall.set()
        def failed():
            detail=h.api(f'/v1/jobs/{job}/renditions/main')
            return detail if next(x for x in detail['destinations'] if x['destination_id']=='primary')['state']=='failed' else None
        failure=wait(failed,8);before=next(x for x in failure['destinations'] if x['destination_id']=='mirror')['packets_written']
        def advancing():
            detail=h.api(f'/v1/jobs/{job}/renditions/main')
            return detail if next(x for x in detail['destinations'] if x['destination_id']=='mirror')['packets_written']>=before+25 else None
        wait(advancing,5)
        require(h.api()['outputs'][0]['rendition_pid']==pid,'primary stall killed rendition')
        final=terminal(h);require(final['state']=='completed' and final['frames_decoded']==frames,final)
        expected=frames-joined['applied_checkpoint']['frame_id']+1
        require(len(decoded_hashes(directory/'mirror.mp4'))==expected,'healthy mirror lost fixed source suffix')
        require(final['shared_pool_occupied_slots']==0,final)
        return dict(passed=True,attach=joined,failed_primary=failure,expected_mirror_frames=expected,final=final)
    finally:sink.close();h.close()


def required_watermark(directory,sample,backend):
    directory.mkdir();h=Harness(directory);png(directory/'mark.png')
    try:
        h.start();h.api(body=config(sample,[output(directory)],backend),expected=202);wait(lambda:h.api().get('packets_out',0)>=8);time.sleep(1.8)
        marked=output(directory,'marked',watermark=dict(enabled=True,resource=dict(uri=str(directory/'mark.png')),startup=dict(required=True,timeout_ms=1500)))
        job,body=topology(h,'add_rendition','marked',spec=marked);added=operation(h,job,body)
        time.sleep(1.6)
        if backend=='process':
            child=next(x for x in h.api()['outputs'] if x['output_id']=='marked');pidfd=os.pidfd_open(child['rendition_pid'])
            try:signal.pidfd_send_signal(pidfd,signal.SIGKILL)
            finally:os.close(pidfd)
            wait(lambda:(child if (child:=next(x for x in h.api()['outputs'] if x['output_id']=='marked'))['state']=='failed' and not child['rendition_pid'] else None))
            job,body=topology(h,'restart_rendition','marked');restarted=operation(h,job,body)
        else:
            job,body=topology(h,'remove_rendition','marked');operation(h,job,body,False)
            marked['output']=str(directory/'marked-again.mp4')
            job,body=topology(h,'add_rendition','marked',spec=marked);restarted=operation(h,job,body)
        return dict(passed=True,added=added,restarted=restarted,detail=h.api(f'/v1/jobs/{job}/renditions/marked'))
    finally:h.close()


def io_timeout(directory,sample,ms):
    directory.mkdir();h=Harness(directory);sink=Sink()
    try:
        h.start();spec=output(directory,output=f'rtmp://127.0.0.1:{sink.port}/live/a',output_format='flv',io_timeout_ms=ms)
        h.api(body=config(sample,[spec],'process'),expected=202);require(sink.connected.wait(5),'handshake never opened')
        final=terminal(h);require(sink.closed.wait(2),'protocol was not closed');require(final['state']=='failed',final)
        require(ms/1000-.1<=sink.duration<=ms/1000+1.5,('io_timeout ignored',ms,sink.duration))
        return dict(passed=True,requested_ms=ms,connection_s=sink.duration,handshake_bytes=sink.bytes,final=final)
    finally:sink.close();h.close()


def finalize_error(directory,sample):
    directory.mkdir();h=Harness(directory,BIN/'rendition-faults',{'NAKI_TEST_RENDITION_FAULT':'trailer_error'})
    try:
        h.start();h.api(body=config(sample,[output(directory,'bad')],'process'),expected=202);wait(lambda:h.api().get('packets_out',0)>=8)
        job,body=topology(h,'add_destination','bad',destination='mirror',spec=dict(output=str(directory/'mirror.mp4'),output_format='mp4'));operation(h,job,body)
        final=terminal(h);require(final['state']=='failed',final)
        destinations=final['outputs'][0]['destinations'];require(len(destinations)==2,destinations)
        require(all(x['state']=='failed' and x['error_code']==-5 for x in destinations),destinations)
        return dict(passed=True,final=final)
    finally:h.close()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--sanitizer',action='store_true');args=parser.parse_args()
    run=_pathlib.Path(tempfile.mkdtemp(prefix='rendition-regressions-',dir=ROOT/'out'));sample=run/'source.ts';frames=400
    command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x192:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-f','mpegts',sample])
    results={}
    def record(name,fn):
        print('RUN',name,flush=True);results[name]=fn();(run/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
    for backend in ('process','thread'):
        record('migrated-'+backend,lambda backend=backend:migrated_primary(run/('migrated-'+backend),sample,backend,frames))
        record('required-'+backend,lambda backend=backend:required_watermark(run/('required-'+backend),sample,backend))
    for ms in (100,1000):record('timeout-'+str(ms),lambda ms=ms:io_timeout(run/('timeout-'+str(ms)),sample,ms))
    record('finalize-error',lambda:finalize_error(run/'finalize-error',sample))
    print(json.dumps(dict(run=str(run),passed=True)),flush=True)


if __name__=='__main__':main()
