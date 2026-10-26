#!/usr/bin/env python3
"""Zero keepalive and input-owner death with real pidfd exit/reap proof."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0,str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import os
from pathlib import Path
import select
import signal
import tempfile
import uuid
from integration.rendition_process_integration import ROOT,ORACLE_FFMPEG,Harness,command,wait,require


def create(h,source,count=2,idle=60000):
    outputs=[dict(id=f'out{i}',output=str(h.run/f'out{i}.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',audio_copy=False) for i in range(count)]
    h.api(body=dict(input=str(source),outputs=outputs,rendition_backend='process',idle_timeout_ms=idle,pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,timestamps={'mode':'source_all'}),expected=202)
    def ready():
        s=h.api();(h.run/'latest-state.json').write_text(json.dumps(s,indent=2)+'\n');require(s['state'] not in ('failed','stopped','completed','completed_partial'),{k:s.get(k) for k in ['state','error','frames_decoded']})
        return s if s.get('packets_out',0)>=8 and all(x.get('rendition_pid') for x in s.get('outputs',[])) else None
    return wait(ready)


def owner_death(directory,source):
    h=Harness(directory);descriptors=[]
    try:
        h.start();state=create(h,source);job=state['job_id'];root=state['supervisor']['worker_pid'];pids={root,state['crash_collector_pid']}
        for branch in state['outputs']:
            detail=h.api(f'/v1/jobs/{job}/renditions/{branch["output_id"]}');pids.update([branch['rendition_pid'],detail['crash_collector_pid']])
        for pid in pids:
            require(pid>1,'missing live holder/collector identity');descriptors.append((pid,os.pidfd_open(pid)))
        signal.pidfd_send_signal(next(fd for pid,fd in descriptors if pid==root),signal.SIGKILL)
        def exited():
            for _,fd in descriptors:
                poll=select.poll();poll.register(fd,select.POLLIN)
                if not poll.poll(0):return None
            return True
        wait(exited,8)
        def reaped():
            return all('\nPid:\t-1\n' in ('\n'+Path(f'/proc/self/fdinfo/{fd}').read_text()) for _,fd in descriptors)
        wait(reaped,8)
        final=wait(lambda:(s if (s:=h.api())['state']=='failed' else None))
        require(final['supervisor']['exit_signal']==signal.SIGKILL,'wrong owner exit cause')
        return dict(initial=state,final=final,holders_and_collectors=sorted(pids),all_exit_proven=True,all_reaped=True)
    finally:
        for _,fd in descriptors:os.close(fd)
        h.close()


def zero_keepalive(directory,source,frames):
    h=Harness(directory)
    try:
        h.start();state=create(h,source,1,0);job=state['job_id']
        body=dict(operation_id=uuid.uuid4().hex,action='remove_rendition',target=dict(component='rendition',output_id='out0'),expected_revision=state['topology_revision'])
        h.api(f'/v1/jobs/{job}/updates',body,202)
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),8)
        require(final['state']=='completed' and not final['outputs'] and final['shared_pool_occupied_slots']==0,final)
        require(8<=final['frames_decoded']<frames,'zero keepalive waited for input EOF')
        return dict(initial=state,final=final,fixed_input_frames=frames)
    finally:h.close()


def main():
    run=Path(tempfile.mkdtemp(prefix='rendition-lifetime-',dir=ROOT/'out'));source=run/'source.ts';frames=150;report={}
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-f','mpegts',source])
    try:
        for name,function in [('owner-death',lambda d:owner_death(d,source)),('zero-keepalive',lambda d:zero_keepalive(d,source,frames))]:
            directory=run/name;directory.mkdir();report[name]=function(directory)
        report['passed']=True
    finally:(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
