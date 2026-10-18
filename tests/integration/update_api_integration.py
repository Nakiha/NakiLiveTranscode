#!/usr/bin/env python3
"""Authoritative typed updates, real mux delivery, recovery and bounded history."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
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
from support.images import png
from support.event_logs import read_events

ROOT=pathlib.Path(__file__).resolve().parents[2]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
RUN = None
from support.update_server import Server, wait, scoped_signal
RESULTS={}



def update(operation,revision,patch,output='main'):
    return {'operation_id':operation,'target':{'output_id':output,'component':'watermark'},'expected_revision':revision,'patch':patch}

def ready_operation(server,job,operation,predicate,timeout=8):
    def read():
        r=server.api(f'/v1/jobs/{job}/operations/{operation}')
        return r if predicate(r) else None
    return wait(read,timeout)


def ordinary(sample,red):
    class Assets(http.server.BaseHTTPRequestHandler):
        requests=0
        def do_GET(self):
            Assets.requests+=1;data=red.read_bytes();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        def log_message(self,*args):pass
    assets=http.server.ThreadingHTTPServer(('127.0.0.1',0),Assets);threading.Thread(target=assets.serve_forever,daemon=True).start()
    server=Server(RUN, 'ordinary')
    try:
        spec={'input':str(sample),'output':str(RUN/'ordinary.ts'),'output_format':'mpegts','pace':True,'frame_log':False}
        accepted=server.api('/v1/jobs',{'operation_id':'create-ordinary','spec':spec},202);job=accepted['job_id'];path=f'/v1/jobs/{job}/updates'
        wait(lambda:server.api().get('packets_out',0)>2)
        a=update('enable',1,{'enabled':True,'resource':{'uri':f'http://127.0.0.1:{assets.server_port}/logo'},'placement':{'size':{'width':16,'height':16},'offset':{'x':3,'y':5}}})
        first=server.api(path,a,(200,202),'attempt-first');assert first['origin_request_id']=='attempt-first' and first['accepted_revision']==2
        written=ready_operation(server,job,'enable',lambda r:r['delivery']['state']=='first_written')
        retries=server.api(path,a,200,'attempt-retry');assert retries['origin_request_id']=='attempt-first' and retries['accepted_revision']==2 and Assets.requests==1
        changed=update('enable',2,{'placement':{'opacity':.3}});server.api(path,changed,409)
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'enable'},409)
        assert server.api()['supervisor']['active']
        b=update('opacity',2,{'placement':{'opacity':.5}});server.api(path,b,(200,202))
        ready_operation(server,job,'opacity',lambda r:r['delivery']['state']=='first_written')
        projection=wait(lambda:(c if (c:=server.api().get('components',{}).get('watermark',{})).get('desired_revision')==3 else None));assert projection['desired']['placement']['offset']=={'x':3,'y':5}
        assert projection['desired']['resource']['uri']==a['patch']['resource']['uri'] and projection['desired_revision']==3
        server.restart();recovered=wait(lambda:(s if (s:=server.api()).get('components',{}).get('watermark',{}).get('effective_revision')==3 else None));assert recovered['job_id']==job
        old=server.api(path,a,200);assert old['accepted_revision']==2 and Assets.requests==1
        server.api(path,update('prepare-fail',3,{'resource':{'uri':str(RUN/'absent.png')}}),(200,202))
        failed=ready_operation(server,job,'prepare-fail',lambda r:r['state']=='failed')
        state=wait(lambda:(c if (c:=server.api().get('components',{}).get('watermark',{})).get('desired_revision')==4 else None));assert state['effective_revision']==3
        assert state['desired']['resource']['uri'].endswith('absent.png')
        server.api(path,update('recover',4,{'resource':{'uri':str(red)}}),(200,202))
        ready_operation(server,job,'recover',lambda r:r['delivery']['state']=='first_written')
        patch=RUN/'cli-update.json';patch.write_text(json.dumps(update('cli-opacity',5,{'placement':{'opacity':.7}})))
        def cli(*args):
            r=subprocess.run([str(BIN/'naki-live-ctl'),'--url',server.base,*args],capture_output=True,timeout=6)
            assert r.returncode==0,(r.returncode,r.stdout,r.stderr);return json.loads(r.stdout)
        assert cli('update',job,str(patch))['accepted_revision']==6
        ready_operation(server,job,'cli-opacity',lambda r:r['delivery']['state']=='first_written')
        assert cli('operation',job,'cli-opacity')['outcome_known'] and cli('job',job)['job_id']==job
        assert cli('stop-job',job,'stop-ordinary')['kind']=='stop'
        wait(lambda:server.api()['state']=='stopped')
        historical=server.api(path,a,200);assert historical['delivery']['first_written']==written['delivery']['first_written']
        assert not list((server.logs/'logs').glob('*.trace.*'))
        events=read_events(server.logs/'logs/events')
        phases=[r for r in events if r.get('role')=='worker' and r.get('operation_id')=='enable']
        for name in ('update_accepted','update_preparing','update_prepared','update_applying','update_applied','update_first_written'):
            rows=[r for r in phases if r['event']==name];assert len(rows)==1,(name,rows)
            row=rows[0];assert row['revision']==2 and row['job_id']==job and row['output_id']=='main' and row['component']=='watermark'
            assert row['origin_request_id']=='attempt-first' and row['create_request_id']==accepted['create_request_id']
        first_write=next(r for r in phases if r['event']=='update_first_written');assert first_write['frame_id']==written['delivery']['first_written']['frame_id']
        RESULTS['real-write-log-off-dedup-sparse-failure-recovery-and-controller-restart']={'first_written':written,'failed':failed,'http_resource_loads':Assets.requests}
    finally:server.close();assets.shutdown();assets.server_close()

def full_table(sample):
    server=Server(RUN, 'full-table')
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-full','spec':{'input':str(sample),'output':str(RUN/'full.ts'),'pace':True}},202)
        job=accepted['job_id'];path=f'/v1/jobs/{job}/updates'
        for i in range(256):
            result=server.api(path,update(f'full-{i}',i+1,{'enabled':False}),(200,202));assert result['accepted_revision']==i+2
        server.api(path,update('overflow',257,{'enabled':False}),429)
        wait(lambda:not server.api('/health')['pool']['recovering'])
        server.restart()
        replay=server.api(path,update('full-0',1,{'enabled':False}),200);assert replay['state'] in ('superseded','applied')
        server.api(path,update('overflow-again',257,{'enabled':False}),429)
        wait(lambda:not server.api('/health')['pool']['recovering'])
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'full-0'},409)
        assert server.api()['supervisor']['active']
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'reserved-stop'},202)
        wait(lambda:server.api()['state']=='stopped')
        final=server.api(f'/v1/jobs/{job}/operations/full-255');assert final['state'] in ('cancelled','applied') and final['outcome_known']
        RESULTS['256-results-recovery-paging-capacity-before-side-effects-and-reserved-stop']=final
    finally:server.close()

def multiple(sample,red):
    server=Server(RUN, 'multiple')
    try:
        outputs=[{'id':name,'output':str(RUN/(name+'.ts')),'output_format':'mpegts','encoder_preset':'ultrafast'} for name in ('main','preview')]
        accepted=server.api('/v1/jobs',{'operation_id':'create-multiple','spec':{'input':str(sample),'outputs':outputs,'pace':True}},202)
        job=accepted['job_id'];path=f'/v1/jobs/{job}/updates'
        wait(lambda:len((branches:=server.api().get('outputs',[])))==2 and all(b.get('packets_out',0)>2 for b in branches))
        patch={'enabled':True,'resource':{'uri':str(red)},'placement':{'size':{'width':16,'height':16}}}
        first=update('shared-id',1,patch);server.api(path,first,(200,202));ready_operation(server,job,'shared-id',lambda r:r['delivery']['state']=='first_written')
        server.api(path,update('shared-id',1,patch,'preview'),409)
        second=update('preview-id',1,patch,'preview');server.api(path,second,(200,202));ready_operation(server,job,'preview-id',lambda r:r['delivery']['state']=='first_written')
        branches=wait(lambda:(b if len(b:=server.api().get('outputs',[]))==2 and all(x.get('components',{}).get('watermark',{}).get('effective_revision')==2 for x in b) else None))
        assert {b['output_id'] for b in branches}=={'main','preview'}
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'stop-multiple'},202);wait(lambda:server.api()['state']=='stopped')
        replacement=server.api('/v1/jobs',{'operation_id':'new-job','spec':{'input':str(sample),'output':str(RUN/'replacement.ts'),'pace':True}},202)
        replay=server.api(path,first,200);assert replay['job_id']==job and server.api()['job_id']==replacement['job_id']
        server.api(path,update('late-new',2,{'enabled':False}),409)
        server.api(f'/v1/jobs/{replacement["job_id"]}/stop',{'operation_id':'stop-new'},202);wait(lambda:server.api()['state']=='stopped')
        RESULTS['shared-store-independent-target-revisions-kind-domain-and-stale-job-replay']=branches
    finally:server.close()

def gate_case(sample,red,name,terminate):
    binaries=RUN/(name+'-bin');binaries.mkdir()
    for exe in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/exe,binaries/exe)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    gate=RUN/(name+'-gate');gate.mkdir()
    server=Server(RUN, name,binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(gate),'NAKI_TEST_MEDIA_HOOK_MODE':'gate_video','NAKI_TEST_MEDIA_HOOK_GATE':str(gate)})
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-'+name,'spec':{'input':str(sample),'output':str(RUN/(name+'.ts')),
            'encoder':'libvvenc','output_format':'mpegts','pace':True,'frame_log':False}},202)
        job=accepted['job_id'];path=f'/v1/jobs/{job}/updates'
        def entered():
            try:return json.loads((gate/'entered.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return None
        checkpoint=wait(entered)
        request=update('held-operation',1,{'enabled':True,'resource':{'uri':str(red)},'placement':{'size':{'width':16,'height':16}}})
        server.api(path,request,(200,202));ready_operation(server,job,'held-operation',lambda r:r['state']=='prepared')
        for _ in range(24):
            operation=server.api(f'/v1/jobs/{job}/operations/held-operation')
            if operation['state']=='applied':break
            old_packet=checkpoint['packet'];(gate/'release').touch()
            checkpoint=wait(lambda:(r if (r:=entered()) and r['packet']>old_packet else None))
        else:raise AssertionError('owner never applied the prepared operation')
        assert operation['delivery']['state']=='pending',operation
        if terminate:
            pid=server.api()['supervisor']['worker_pid'];scoped_signal(pid,signal.SIGKILL)
            wait(lambda:not server.api()['supervisor']['active'])
            unknown=server.api(f'/v1/jobs/{job}/operations/held-operation')
            assert not unknown['outcome_known'] and unknown['state']=='unknown' and unknown['reason']=='operation_outcome_unknown'
            assert unknown['applied_checkpoint']==operation['applied_checkpoint'] and unknown['last_observed']['delivery']['state']=='pending'
            RESULTS['worker-death-with-real-applied-frame-and-unconfirmed-delivery']=unknown
        else:
            (gate/'fail').touch();wait(lambda:server.api()['state']=='failed')
            final=server.api(f'/v1/jobs/{job}/operations/held-operation')
            assert final['state']=='applied' and final['delivery']['state']=='not_written' and final['delivery']['inflight_frame_leases']==0,final
            assert final['applied_checkpoint']==operation['applied_checkpoint']
            RESULTS['native-output-failure-retains-applied-and-closes-real-frame-references']=final
    finally:
        (gate/'off').touch();server.close()

def reordered_delivery(sample,red):
    binaries=RUN/'reorder-bin';binaries.mkdir()
    for exe in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/exe,binaries/exe)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    gate=RUN/'reorder-gate';gate.mkdir()
    server=Server(RUN, 'reorder',binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(gate),'NAKI_TEST_MEDIA_HOOK_MODE':'gate_video','NAKI_TEST_MEDIA_HOOK_GATE':str(gate)})
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-reorder','spec':{'input':str(sample),'output':str(RUN/'reorder.ts'),
            'encoder':'libvvenc','output_format':'mpegts','frame_log':True}},202)
        job=accepted['job_id'];path=f'/v1/jobs/{job}/updates'
        def entered():
            try:return json.loads((gate/'entered.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return None
        checkpoint=wait(entered);operations=[];different=False
        def release():
            nonlocal checkpoint
            packet=checkpoint['packet'];(gate/'release').touch()
            checkpoint=wait(lambda:(r if (r:=entered()) and r['packet']>packet else None))
        for revision in range(1,9):
            operation_id=f'reorder-{revision}'
            patch={'enabled':True,'resource':{'uri':str(red)},'placement':{'size':{'width':16,'height':16},'opacity':.5+revision*.02}}
            server.api(path,update(operation_id,revision,patch),(200,202));ready_operation(server,job,operation_id,lambda r:r['state']=='prepared')
            for _ in range(32):
                operation=server.api(f'/v1/jobs/{job}/operations/{operation_id}')
                if operation['state']=='applied':break
                release()
            else:raise AssertionError('revision did not reach owner')
            applied=operation['applied_checkpoint']['frame_id']
            while checkpoint['frame_id']<applied:release()
            first=checkpoint['frame_id'];release()
            written=ready_operation(server,job,operation_id,lambda r:r['delivery']['state']=='first_written')
            assert written['delivery']['first_written']['frame_id']==first,(written,first)
            assert written['applied_checkpoint']['frame_id']==applied
            different|=first!=applied;operations.append(written)
        (gate/'off').touch();server.api(f'/v1/jobs/{job}/stop',{'operation_id':'stop-reorder'},202);wait(lambda:server.api()['state']=='stopped')
        state=server.api();prefix=pathlib.Path(state['log_path'])
        rows=[json.loads(line) for f in sorted(prefix.parent.glob(prefix.name+'.*.jsonl')) for line in f.read_text().splitlines()]
        frames=[r for r in rows if r.get('event')=='frame'];maximum=0;late=[]
        for row in frames:
            revision=row['watermark_revision']
            if 2<=revision<maximum:late.append(row)
            maximum=max(maximum,revision)
        assert different and late,{'different_first_written_frame':different,'late_old_revision':late,'operations':operations}
        for operation in operations:
            retained=server.api(f'/v1/jobs/{job}/operations/{operation["operation_id"]}')
            assert retained['delivery']['first_written']==operation['delivery']['first_written']
        RESULTS['real-vvc-b-picture-old-revision-late-write-and-later-frame-first-written']={'operations':operations,'late_old_revision_frames':late}
    finally:(gate/'off').touch();server.close()

def lost_event_logs(sample,red):
    server=Server(RUN, 'event-loss',environment={'LD_PRELOAD':str(BIN/'libnaki-event-write-fault.so'),'NAKI_TEST_EVENT_WRITE_FAILURE':'1'})
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-event-loss','spec':{'input':str(sample),'output':str(RUN/'event-loss.ts'),'pace':True,'frame_log':False}},202)
        job=accepted['job_id'];wait(lambda:server.api().get('packets_out',0)>2)
        server.api(f'/v1/jobs/{job}/updates',update('without-events',1,{'enabled':True,'resource':{'uri':str(red)}}),(200,202))
        written=ready_operation(server,job,'without-events',lambda r:r['delivery']['state']=='first_written')
        logging=wait(lambda:(s if (s:=server.api().get('logging',{})).get('io_errors',0)>0 and s.get('dropped_status',0)>0 else None))
        assert not any(row.get('operation_id')=='without-events' for row in read_events(server.logs/'logs/events'))
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'stop-without-events'},202);wait(lambda:server.api()['state']=='stopped')
        final=server.api(f'/v1/jobs/{job}/operations/without-events');assert final['delivery']['first_written']==written['delivery']['first_written']
        assert server.api()['supervisor']['exit_code']==0
        RESULTS['event-write-loss-does-not-block-media-or-authoritative-checkpoints']={'logging':logging,'operation':final}
    finally:server.close()

def rejected_legacy_reservations(sample):
    server=Server(RUN, 'legacy-rejections')
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-legacy-rejections','spec':{'input':str(sample),'output':str(RUN/'legacy-rejections.ts'),'pace':True}},202)
        job=accepted['job_id'];wait(lambda:server.api().get('packets_out',0)>2)
        for _ in range(256):
            rejected=server.api('/v1/job/watermark',{'job_id':job,'revision':3,'watermark':{'enabled':False}},409)
            assert not rejected['accepted'] and 'operation' not in rejected
        accepted=server.api('/v1/job/watermark',{'job_id':job,'revision':2,'watermark':{'enabled':False}},202)
        assert accepted['accepted'] and accepted['operation']['accepted_revision']==2
        server.api(f'/v1/jobs/{job}/updates',update('typed-after-rejections',2,{'enabled':False}),(200,202))
        server.api(f'/v1/jobs/{job}/stop',{'operation_id':'stop-legacy-rejections'},202);wait(lambda:server.api()['state']=='stopped')
        RESULTS['256-explicit-legacy-rejections-release-controller-reservations']=accepted
    finally:server.close()

def final_before_pending(sample,red,written):
    name='message-order-'+('written' if written else 'not-written')
    binaries=RUN/(name+'-bin');binaries.mkdir()
    for exe in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/exe,binaries/exe)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    gate=RUN/(name+'-gate');gate.mkdir();order=RUN/(name+'-order');order.mkdir()
    server=Server(RUN, name,binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(gate),'NAKI_TEST_MEDIA_HOOK_MODE':'gate_video','NAKI_TEST_MEDIA_HOOK_GATE':str(gate),
        'NAKI_TEST_OPERATION_ORDER_DIR':str(order),'NAKI_TEST_OPERATION_ORDER_ID':'ordered'})
    pool=concurrent.futures.ThreadPoolExecutor(max_workers=1)
    try:
        accepted=server.api('/v1/jobs',{'operation_id':'create-'+name,'spec':{'input':str(sample),'output':str(RUN/(name+'.ts')),'encoder':'libvvenc','output_format':'mpegts','pace':True}},202)
        job=accepted['job_id']
        def entered():
            try:return json.loads((gate/'entered.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return None
        checkpoint=wait(entered)
        server.api(f'/v1/jobs/{job}/updates',update('ordered',1,{'enabled':True,'resource':{'uri':str(red)}}),(200,202))
        for _ in range(32):
            request=pool.submit(server.api,f'/v1/jobs/{job}/operations/ordered')
            wait(lambda:(order/'captured-pending.json').exists() or request.done())
            if (order/'captured-pending.json').exists():break
            observed=request.result();assert observed['state'] in ('accepted','preparing','prepared','applying'),observed
            packet=checkpoint['packet'];(gate/'release').touch()
            checkpoint=wait(lambda:(r if (r:=entered()) and r['packet']>packet else None))
        else:raise AssertionError('did not capture applied/pending RPC')
        captured=json.loads((order/'captured-pending.json').read_text())['operation']
        assert captured['delivery']['state']=='pending' and captured['applied_checkpoint']
        if written:
            (gate/'off').touch()
            wait(lambda:any(e.get('event')=='update_first_written' and e.get('operation_id')=='ordered' for e in read_events(server.logs/'logs/events')))
            server.api(f'/v1/jobs/{job}/stop',{'operation_id':'stop-'+name},202)
        else:(gate/'fail').touch()
        response=request.result(timeout=4);assert response['result_version']>captured['result_version']
        wait(lambda:server.api()['state']==('stopped' if written else 'failed'))
        wait(lambda:(order/'sent-old-pending.json').exists())
        sent_page=json.loads((order/'sent-final-page.json').read_text())
        final=next(r for r in sent_page['operations'] if r['operation_id']=='ordered')
        assert final['result_version']>captured['result_version']
        assert final['delivery']['state']==('first_written' if written else 'not_written')
        assert response['result_version']==final['result_version'] and response['delivery']==final['delivery']
        historical=server.api(f'/v1/jobs/{job}/operations/ordered')
        assert historical['outcome_known'] and historical['result_source']=='controller_history',historical
        assert historical['result_version']==final['result_version'] and historical['delivery']==final['delivery']
        assert historical['applied_checkpoint']==captured['applied_checkpoint'] and historical['retention_pins']==final['retention_pins']
        RESULTS[name]={'captured_pending':captured,'final_export':final,'after_worker_exit':historical}
    finally:
        (gate/'off').touch();server.close();pool.shutdown(wait=True)

def run(case=None):
    global RUN
    RUN=pathlib.Path(tempfile.mkdtemp(prefix='update-api-',dir=ROOT/'out'))
    sample=RUN/'input.mp4';red=RUN/'logo.png';png(red)
    subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x90:rate=25','-frames:v','1500','-c:v','libx264','-threads','2','-bf','3',str(sample)],check=True,timeout=25)
    if case:
        if case=='event-loss':lost_event_logs(sample,red)
        else:
            assert case=='review-p2';rejected_legacy_reservations(sample);final_before_pending(sample,red,True);final_before_pending(sample,red,False)
    else:
        ordinary(sample,red);multiple(sample,red);full_table(sample);gate_case(sample,red,'unknown',True);gate_case(sample,red,'not-written',False);reordered_delivery(sample,red);lost_event_logs(sample,red)
        rejected_legacy_reservations(sample);final_before_pending(sample,red,True);final_before_pending(sample,red,False)
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','cases':RESULTS},indent=2)+'\n')
    print('PASS authoritative update API, real delivery, bounded recovery and unknown outcomes:',RUN,flush=True)

if __name__=='__main__':run(os.environ.get('NAKI_UPDATE_TEST_CASE'))
