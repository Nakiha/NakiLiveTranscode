#!/usr/bin/env python3
"""Controller health, actual worker/control stalls, waiting input and scoped progress."""
import concurrent.futures
import http.server
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from update_api_integration import Server,wait,scoped_signal

ROOT=pathlib.Path(__file__).resolve().parents[1];BIN=ROOT/'out/bin'
RUN=pathlib.Path(tempfile.mkdtemp(prefix='health-progress-',dir=ROOT/'out'));RESULTS={}
def command(args):subprocess.run(list(map(str,args)),check=True,capture_output=True,timeout=30)
def finish(server,job,operation):
    server.api(f'/v1/jobs/{job}/stop',{'operation_id':operation},202)
    return wait(lambda:(s if (s:=server.api())['state']=='stopped' else None))
def closed_progress(progress):
    assert progress['oldest_video_inflight_us'] is None,progress
    assert not progress['tracked_video_packets'] and not progress['tracked_video_frames'] and not progress['retained_branch_frames'],progress
def busy_and_control_stalls(sample):
    server=Server('health-busy')
    try:
        idle=wait(lambda:(h if (h:=server.api('/health'))['admission']['open'] else None))
        assert idle['liveness']['alive'] and idle['readiness']['ready'] and idle['resource_observation']['controller_fd_count']>0
        job=server.api('/v1/jobs',{'operation_id':'health-busy','spec':{'input':str(sample),'output':str(RUN/'busy.ts'),'pace':True}},202)['job_id']
        live=wait(lambda:(s if (s:=server.api()).get('packets_out',0)>5 else None))
        assert server.api('/health/ready')['readiness']['ready']
        assert not server.api('/health/admission',code=503)['admission']['open']
        progress=live['progress']
        for field in ('last_input','last_decoded','last_encoded','last_written'):
            assert progress[field+'_us']>0 and progress[field+'_age_ms']>=0,progress
        freshness=live['freshness'];assert freshness['worker_generated_monotonic_us']<=freshness['controller_received_monotonic_us']<=freshness['controller_observed_monotonic_us']
        assert freshness['worker_snapshot_generation']>0 and not freshness['stale']
        pid=live['supervisor']['worker_pid'];scoped_signal(pid,signal.SIGSTOP)
        try:
            stale=wait(lambda:(h if (h:=server.api('/health/ready',code=(200,503)))['readiness']['reason']=='worker_control_stale' else None),4)
            assert stale['liveness']['alive'] and stale['freshness']['stale'] and stale['freshness']['receipt_age_ms']>=2000,stale
            assert server.api('/health/live')['liveness']['alive']
            frozen=server.api();time.sleep(.12);again=server.api()
            assert again['snapshot_generation']==frozen['snapshot_generation'] and again['freshness']['receipt_age_ms']>frozen['freshness']['receipt_age_ms']
            assert again['supervisor']['active']
        finally:scoped_signal(pid,signal.SIGCONT)
        resumed=wait(lambda:(h if (h:=server.api('/health/ready',code=(200,503)))['readiness']['ready'] else None))
        wait(lambda:server.api().get('snapshot_generation',0)>frozen['snapshot_generation'])
        # A stopped HTTP event loop cannot answer liveness, while its independent
        # media worker continues. No status body is invented during the pause.
        before=(RUN/'busy.ts').stat().st_size;scoped_signal(server.process.pid,signal.SIGSTOP)
        try:
            try:
                urllib.request.urlopen(server.base+'/health/live',timeout=.3).close()
                raise AssertionError('stopped controller answered HTTP liveness')
            except (TimeoutError,socket.timeout,urllib.error.URLError):pass
            time.sleep(.15);assert (RUN/'busy.ts').stat().st_size>before
        finally:scoped_signal(server.process.pid,signal.SIGCONT)
        assert server.api('/health/live')['liveness']['alive']
        final=finish(server,job,'stop-health-busy');closed_progress(final['progress'])
        assert final['progress']['phase']=='stopped' and server.api('/health/ready')['readiness']['ready']
        for path in ('/health/live','/health/ready','/health/admission'):server.api(path,{},405)
        RESULTS['busy-remains-ready-worker-stale-and-controller-liveness-boundaries']={'idle':idle,'busy':live,'worker_stopped':stale,'resumed':resumed,'final':final}
    finally:server.close()
def held_encoded_frame(sample):
    binaries=RUN/'gate-bin';binaries.mkdir();gate=RUN/'gate';gate.mkdir()
    for name in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/name,binaries/name)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    server=Server('health-held-frame',binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(gate),'NAKI_TEST_MEDIA_HOOK_MODE':'gate_video','NAKI_TEST_MEDIA_HOOK_GATE':str(gate)})
    try:
        job=server.api('/v1/jobs',{'operation_id':'health-held-frame','spec':{'input':str(sample),'output':str(RUN/'held.ts'),'pace':True}},202)['job_id']
        wait(lambda:(gate/'entered.json').exists())
        first=wait(lambda:(s if (s:=server.api()).get('progress',{}).get('last_encoded_us') else None))
        assert first['progress']['last_written_us'] is None and first['progress']['oldest_video_inflight_us']>0,first
        time.sleep(.35);second=server.api();assert second['snapshot_generation']>first['snapshot_generation']
        assert second['progress']['oldest_video_inflight_age_ms']>first['progress']['oldest_video_inflight_age_ms']
        assert server.api('/health/ready')['readiness']['ready']
        (gate/'off').touch();wait(lambda:server.api().get('packets_out',0)>2)
        final=finish(server,job,'stop-held');closed_progress(final['progress'])
        RESULTS['encoded-before-write-with-unconditional-bounded-video-reference-observation']={'held':second,'final':final}
    finally:(gate/'off').touch();server.close()
def waiting_input(sample):
    transport=RUN/'input.ts';command(['ffmpeg','-v','error','-i',sample,'-c','copy','-muxdelay','0','-muxpreload','0','-f','mpegts',transport])
    release=threading.Event();sent=threading.Event();data=transport.read_bytes()
    class Source(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def do_GET(self):
            self.send_response(200);self.send_header('Content-Type','video/mp2t');self.send_header('Transfer-Encoding','chunked');self.send_header('Connection','close');self.end_headers()
            try:
                self.wfile.write(f'{len(data):x}\r\n'.encode()+data+b'\r\n');self.wfile.flush();sent.set();release.wait(8)
                self.wfile.write(b'0\r\n\r\n');self.wfile.flush()
            except (BrokenPipeError,ConnectionResetError):pass
            self.close_connection=True
        def log_message(self,*args):pass
    source=http.server.ThreadingHTTPServer(('127.0.0.1',0),Source);threading.Thread(target=source.serve_forever,daemon=True).start()
    server=Server('health-waiting-input')
    try:
        outputs=[{'id':f'branch-{i}','output':str(RUN/f'waiting-{i}.ts'),'output_format':'mpegts','encoder_preset':'ultrafast'} for i in range(2)]
        job=server.api('/v1/jobs',{'operation_id':'health-waiting','spec':{'input':f'http://127.0.0.1:{source.server_port}/input.ts','outputs':outputs}},202)['job_id']
        assert sent.wait(3)
        observed=wait(lambda:(s if (s:=server.api()).get('progress',{}).get('phase')=='waiting_input' and len(s.get('outputs',[]))==2 and all(b['progress']['last_written_us'] for b in s['outputs']) else None),5)
        assert server.api('/health/ready')['readiness']['ready'] and not server.api('/health/admission',code=503)['admission']['open']
        assert all(b['progress']['last_input_us'] and b['progress']['last_decoded_us'] and b['progress']['last_encoded_us'] for b in observed['outputs'])
        time.sleep(.3);assert server.api('/health/ready')['readiness']['ready']
        # Exercise management of a source that is still waiting, rather than
        # interpreting an HTTP stream closure as successful media EOF.
        final=finish(server,job,'stop-waiting-input')
        assert final['supervisor']['exit_code']==0,final
        for branch in final['outputs']:
            assert branch['packets_out']>0 and not branch['queue_items'] and not branch['inflight_frames'],branch
            closed_progress(branch['progress'])
        RESULTS['real-open-http-input-wait-with-two-live-output-owners']={'waiting':observed,'termination':'explicit_stop_while_source_remains_open','final':final}
    finally:release.set();server.close();source.shutdown();source.server_close()
def run():
    sample=RUN/'input.mp4';command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v','300','-c:v','libx264','-threads','2','-bf','3',sample])
    busy_and_control_stalls(sample);held_encoded_frame(sample);waiting_input(sample)
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','cases':RESULTS},indent=2)+'\n')
    print('PASS busy/recovery health boundaries, actual control/media stalls, waiting input and bounded progress:',RUN,flush=True)
if __name__=='__main__':run()
