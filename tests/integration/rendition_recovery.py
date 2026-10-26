#!/usr/bin/env python3
"""Real holder restart / controller recovery with a fixed shared-source oracle."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
from support.images import png
import json
import os
from pathlib import Path
import signal
import tempfile
import time
import uuid
from integration.rendition_process_integration import ROOT, BIN, ORACLE_FFMPEG, Harness, command, wait, require, frame_rows, decoded_hashes


def send_signal(pid, number):
    fd = os.pidfd_open(pid)
    try:
        signal.pidfd_send_signal(fd, number)
    finally:
        os.close(fd)


def run_case(directory, sample, frames, automatic):
    fault = automatic
    harness = Harness(directory, BIN / 'rendition-faults' if fault else BIN,
                      {'NAKI_TEST_RENDITION_FAULT': 'write_shared'} if fault else None)
    records = {}
    try:
        harness.start()
        mark=directory/'mark.png';png(mark)
        specs = [dict(id=name, output=str(directory / (name + '.mp4')), output_format='mp4',
                      width=160, height=96, encoder_preset='ultrafast', audio_copy=False, max_lag_ms=10000)
                 for name in ['bad', 'good']]
        config = dict(input=str(sample), outputs=specs, rendition_backend='process', pace=True,
                      max_threads=2, min_threads=2, decoder_auto_threads=False, frame_log=True,
                      rendition_auto_restart=automatic, rendition_restart_limit=2, rendition_restart_backoff_ms=100,
                      timestamps={'mode': 'source_all'})
        harness.api(body=config, expected=202)
        def ready():
            s=harness.api()
            return s if next((b.get('packets_out',0) for b in s.get('outputs',[]) if b['output_id']=='good'),0)>=8 else None
        state=wait(ready)
        job = state['job_id'];root_pid=state['supervisor']['worker_pid']
        good_pid=next(b['rendition_pid'] for b in state['outputs'] if b['output_id']=='good')
        if not automatic:
            bad=next(b for b in state['outputs'] if b['output_id']=='bad');old_generation=bad['rendition_generation']
            watermark=dict(operation_id=uuid.uuid4().hex,target=dict(component='watermark',output_id='bad'),expected_revision=1,expected_generation=old_generation,patch=dict(enabled=True,resource=dict(uri=str(mark)),placement=dict(opacity=.5,size=dict(width=8,height=8))))
            harness.api(f'/v1/jobs/{job}/updates',watermark,202)
            records['watermark']=wait(lambda:(op if (op:=harness.api(f'/v1/jobs/{job}/operations/{watermark["operation_id"]}'))['state']=='applied' and op['delivery']['state']=='first_written' else None))
            send_signal(bad['rendition_pid'], signal.SIGKILL)
            failed=wait(lambda: (b if not (b := next(x for x in harness.api()['outputs'] if x['output_id']=='bad'))['rendition_pid'] and b['state']=='failed' else None))
            time.sleep(.2)
            require(not next(b for b in harness.api()['outputs'] if b['output_id']=='bad')['rendition_pid'], 'manual default unexpectedly restarted')
            state=harness.api();body=dict(operation_id=uuid.uuid4().hex,action='restart_rendition',target=dict(component='rendition',output_id='bad'),expected_revision=state['topology_revision'],expected_generation=old_generation)
            records['accepted']=harness.api(f'/v1/jobs/{job}/updates',body,202)
            def applied():
                op=harness.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}')
                require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and op['delivery']['state']=='first_written' else None
            records['restart']=wait(applied)
            def restarted():
                s=harness.api();b=next(b for b in s['outputs'] if b['output_id']=='bad')
                return s if b['rendition_pid'] and b['rendition_generation']>old_generation else None
            state=wait(restarted);bad=next(b for b in state['outputs'] if b['output_id']=='bad')
            require(bad['rendition_generation']>old_generation and bad['rendition_pid']!=failed.get('rendition_pid'),bad)
            records['new_generation']=bad['rendition_generation'];records['new_log']=bad['log_path']
            detail=harness.api(f'/v1/jobs/{job}/renditions/bad')
            records['actual_output']=detail['actual_output']
            require(detail['components']['watermark']['desired']['enabled'] and detail['components']['watermark']['desired']['placement']['opacity']==.5,'known desired watermark was lost on restart')
            require(detail['detail_scope']=='full' and detail['rendition_generation']==bad['rendition_generation'],detail)
            stale=dict(body, operation_id=uuid.uuid4().hex, expected_revision=state['topology_revision'])
            harness.api(f'/v1/jobs/{job}/updates',stale,409)
            # Lose just the controller. Actual holder identities and operation
            # history must be recovered before admission opens again.
            send_signal(harness.server.pid,signal.SIGKILL);harness.server.wait(timeout=5)
            harness.start(recovering=True);recovered=wait(lambda:(s if (s:=harness.api()).get('outputs') and all(b.get('rendition_pid') for b in s['outputs']) else None))
            (directory/'before-recovery.json').write_text(json.dumps(state,indent=2)+'\n');(directory/'after-recovery.json').write_text(json.dumps(recovered,indent=2)+'\n')
            require(recovered['supervisor']['worker_pid']==root_pid,'shared decoder replaced on controller recovery')
            require({b['output_id']:b['rendition_pid'] for b in recovered['outputs']}=={b['output_id']:b['rendition_pid'] for b in state['outputs']},'rendition replaced on controller recovery')
            replay=harness.api(f'/v1/jobs/{job}/updates',body,(200,202))
            require(replay['state']=='applied' and replay['delivery']['state']=='first_written',replay)
            records['recovered']=recovered
        final=wait(lambda: (s if (s := harness.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        require(final['state']=='completed_partial' if automatic else final['state']=='completed', (automatic, final['state'],final.get('error')))
        require(final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,'decode/reclaim incomplete')
        good=next(b for b in final['outputs'] if b['output_id']=='good');bad=next(b for b in final['outputs'] if b['output_id']=='bad')
        require(good['packets_out']==frames and good['state']=='completed',good)
        rows=frame_rows(good['log_path']);require([r['frame_id'] for r in rows]==list(range(1,frames+1)),'healthy source order changed')
        require(len(decoded_hashes(specs[1]['output']))==frames,'independent healthy decode incomplete')
        if automatic:
            require(bad['automatic_restart_attempts']==2 and bad['state']=='failed' and not bad['rendition_pid'],bad)
        else:
            rows=frame_rows(records['new_log']);expected=list(range(records['restart']['applied_checkpoint']['frame_id'],frames+1))
            require([r['frame_id'] for r in rows]==expected,'restart lost/repeated source frames')
            require(rows[0]['output_key']==1,'new encoder did not start with key packet')
            require(len(decoded_hashes(records['actual_output']))==len(expected),'restart output is not independently decodable')
        records.update(final=final, healthy_pid=good_pid, expected_frames=frames, automatic=automatic)
        return records
    finally:
        harness.close()


def main():
    run=Path(tempfile.mkdtemp(prefix='rendition-recovery-',dir=ROOT/'out'));sample=run/'source.ts';frames=400
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-f','mpegts',sample])
    report={}
    try:
        for automatic in [False,True]:
            directory=run/('automatic' if automatic else 'manual');directory.mkdir()
            report[directory.name]=run_case(directory,sample,frames,automatic)
        report['passed']=True
    finally:
        (run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
