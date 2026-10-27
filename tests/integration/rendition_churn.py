#!/usr/bin/env python3
"""Repeated actual attach/drain/reap, bounded resources and topology takeover."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0,str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import json
import os
from pathlib import Path
import signal
import tempfile
import uuid
from integration.rendition_process_integration import ROOT, ORACLE_FFMPEG, Harness, command, wait, require, frame_rows, decoded_hashes
from integration.rendition_matrix import resources,check_resources


def run_case(directory,source,frames,backend,cycles):
    h=Harness(directory);events=[];samples=[]
    try:
        h.start();base=dict(id='base',output=str(directory/'base.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',encoder_threads=1,audio_copy=False)
        h.api(body=dict(input=str(source),outputs=[base],rendition_backend=backend,pace=True,decoder_pump=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        state=wait(lambda:(s if (s:=h.api()).get('packets_out',0)>=8 else None));job=state['job_id'];root=state['supervisor']['worker_pid'];base_pid=state['outputs'][0]['rendition_pid']
        baseline=dict(fds=len(list(Path(f'/proc/{root}/fd').iterdir())),threads=resources([root])['threads']);last_generation=0
        def update(action,spec=None):
            current=h.api();body=dict(operation_id=uuid.uuid4().hex,action=action,expected_revision=current['topology_revision'],target=dict(component='rendition',output_id='hot'))
            if spec:body['spec']=spec
            h.api(f'/v1/jobs/{job}/updates',body,202)
            def ready():
                op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and (action=='remove_rendition' or op['delivery']['state']=='first_written') else None
            return body,wait(ready)
        for index in range(cycles):
            spec=dict(base,id='hot',output=str(directory/f'hot-{index}.mp4'));add,attached=update('add_rendition',spec)
            def hot_state():
                s=h.api();hot=next((b for b in s['outputs'] if b['output_id']=='hot'),None)
                return s if hot and hot['rendition_pid'] and hot['packets_out']>=2 else None
            state=wait(hot_state);hot=next(b for b in state['outputs'] if b['output_id']=='hot')
            require(hot['rendition_generation']>last_generation,'global holder generation ABA');last_generation=hot['rendition_generation']
            require(next(b['rendition_pid'] for b in state['outputs'] if b['output_id']=='base')==base_pid,'existing encoder restarted')
            if index==cycles//2:
                expected={b['output_id']:(b['rendition_pid'],b['rendition_generation']) for b in state['outputs']}
                fd=os.pidfd_open(h.server.pid)
                try:signal.pidfd_send_signal(fd,signal.SIGKILL)
                finally:os.close(fd)
                h.server.wait(timeout=5);h.start(recovering=True)
                recovered=wait(lambda:(s if len((s:=h.api()).get('outputs',[]))==2 and all(b.get('rendition_pid') for b in s['outputs']) else None))
                require({b['output_id']:(b['rendition_pid'],b['rendition_generation']) for b in recovered['outputs']}==expected,'controller restored original config rather than actual topology')
                require(recovered['supervisor']['worker_pid']==root,'decoder replaced on takeover')
                replay=h.api(f'/v1/jobs/{job}/updates',add,(200,202));require(replay['applied_checkpoint']==attached['applied_checkpoint'] and replay['delivery']['state']=='first_written',replay)
            _,detached=update('remove_rendition');wait(lambda:len(h.api()['outputs'])==1)
            rows=frame_rows(hot['log_path']);expected=list(range(attached['applied_checkpoint']['frame_id'],detached['applied_checkpoint']['frame_id']+1))
            require([r['frame_id'] for r in rows]==expected,'churn hot interval loss/repeat/order')
            require(len(decoded_hashes(spec['output']))==len(expected),'independent churn suffix decode')
            sample=resources([root]);sample['root_fds']=len(list(Path(f'/proc/{root}/fd').iterdir()));samples.append(sample)
            require(sample['root_fds']==baseline['fds'],('input owner FD leak',index,baseline,sample))
            require(sample['threads']==baseline['threads'],('holder/codec/logger thread leak',index,baseline,sample))
            if len(samples)>=10:require(max(s['pss_kib'] for s in samples[-5:])-max(s['pss_kib'] for s in samples[5:10])<16384,('non-plateau PSS growth',samples))
            events.append(dict(add=attached,remove=detached,expected_frames=len(expected),generation=last_generation))
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),60)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,(final['state'],final.get('error')))
        healthy=final['outputs'][0];rows=frame_rows(healthy['log_path']);require([r['frame_id'] for r in rows]==list(range(1,frames+1)),'healthy churn frames')
        require(len(decoded_hashes(base['output']))==frames,'healthy independent fixed count')
        return dict(backend=backend,cycles=cycles,expected_frames=frames,baseline=baseline,samples=samples,events=events,final=final)
    finally:h.close()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--backend',choices=['process','thread','both'],default='both');parser.add_argument('--cycles',type=int,default=20);args=parser.parse_args();check_resources();require(1<=args.cycles<=40,'cycles1..40')
    run=Path(tempfile.mkdtemp(prefix='rendition-churn-',dir=ROOT/'out'));source=run/'source.ts';frames=1000;report={}
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','7','-f','mpegts',source])
    try:
        for backend in (['process','thread'] if args.backend=='both' else [args.backend]):
            directory=run/backend;directory.mkdir();report[backend]=run_case(directory,source,frames,backend,args.cycles)
        report['passed']=True
    finally:(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
