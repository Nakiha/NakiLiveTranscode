#!/usr/bin/env python3
"""One encoder, independent mux destinations, verified IDR gates and CAS."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
from pathlib import Path
import tempfile
import socket
import threading
import uuid
from integration.rendition_recovery import png
from integration.rendition_process_integration import ROOT, ORACLE_FFMPEG, Harness, command, wait, require, frame_rows, decoded_hashes


def survivor_case(directory, sample, frames):
    directory.mkdir();h=Harness(directory);report={}
    try:
        source=directory/'source.mp4'
        command([ORACLE_FFMPEG,'-v','error','-i',sample,'-f','lavfi','-i','sine=frequency=660:sample_rate=48000','-t',str(frames/25),'-map','0:v:0','-map','1:a:0','-c:v','copy','-c:a','aac','-ac','1',source])
        h.start();mark=directory/'mark.png';png(mark)
        h.api(body=dict(input=str(source),outputs=[dict(id='main',output=str(directory/'primary.flv'),output_format='flv',width=160,height=96,encoder_preset='ultrafast',audio_copy=True)],rendition_backend='process',pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        initial=wait(lambda:(s if (s:=h.api()).get('packets_out',0)>=8 else None));job=initial['job_id'];generation=initial['outputs'][0]['rendition_generation'];pid=initial['outputs'][0]['rendition_pid']
        def topology(action,id,spec=None):
            body=dict(operation_id=uuid.uuid4().hex,action=action,target=dict(component='destination',output_id='main',destination_id=id),expected_revision=h.api()['topology_revision'],expected_generation=generation)
            if spec:body['spec']=spec
            h.api(f'/v1/jobs/{job}/updates',body,202)
            def poll():
                op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and (action=='remove_destination' or op['delivery']['state']=='first_written') else None
            return wait(poll)
        mirror=topology('add_destination','mirror',dict(output=str(directory/'mirror.flv'),output_format='flv'))
        removed=topology('remove_destination','primary')
        update=dict(operation_id=uuid.uuid4().hex,target=dict(component='watermark',output_id='main'),expected_revision=1,expected_generation=generation,patch=dict(enabled=True,resource=dict(uri=str(mark)),placement=dict(opacity=.5,size=dict(width=8,height=8))))
        h.api(f'/v1/jobs/{job}/updates',update,202)
        def delivered():
            op=h.api(f'/v1/jobs/{job}/operations/{update["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
            return op if op['state']=='applied' and op['delivery']['state']=='first_written' else None
        watermark=wait(delivered);detail=h.api(f'/v1/jobs/{job}/renditions/main')
        require(detail['rendition_pid']==pid and detail['delivery_statistics_scope']=='primary_destination',detail)
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,final)
        expected=frames-mirror['applied_checkpoint']['frame_id']+1
        require(len(decoded_hashes(directory/'mirror.flv'))==expected,('sole surviving mirror count',expected))
        destination=next(d for d in final['outputs'][0]['destinations'] if d['destination_id']=='mirror')
        require(destination['packets_written']==expected and destination['bytes_written']>=(directory/'mirror.flv').stat().st_size and destination['audio_packets_written']>0,destination)
        packets=json.loads(command(['ffprobe','-v','error','-show_packets','-show_entries','packet=codec_type,pts_time','-of','json',directory/'mirror.flv']))['packets'];audio=[float(x['pts_time']) for x in packets if x['codec_type']=='audio'];video=[float(x['pts_time']) for x in packets if x['codec_type']=='video']
        require(audio and audio==sorted(audio) and abs(audio[0]-video[0])<=.1 and abs(audio[-1]-video[-1])<=.1,'surviving destination A/V gate or tail drift')
        require(len(audio)==destination['audio_packets_written'],'per-destination audio actual-write counter')
        report.update(passed=True,mirror=mirror,removed=removed,watermark=watermark,expected_frames=expected,final=final)
    finally:
        h.close();(directory/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def empty_destination_case(directory, sample, frames, backend):
    directory.mkdir();h=Harness(directory);report={}
    try:
        h.start()
        h.api(body=dict(input=str(sample),outputs=[dict(id='main',output=str(directory/'primary.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',audio_copy=False)],rendition_backend=backend,pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        initial=wait(lambda:(s if (s:=h.api()).get('packets_out',0)>=8 else None));job=initial['job_id'];generation=initial['outputs'][0]['rendition_generation']
        def request(action, id=None, spec=None):
            body=dict(operation_id=uuid.uuid4().hex,action=action,expected_revision=h.api()['topology_revision'],expected_generation=generation,target=dict(component='destination' if id else 'rendition',output_id='main'))
            if id:body['target']['destination_id']=id
            if spec:body['spec']=spec
            h.api(f'/v1/jobs/{job}/updates',body,202)
            wait(lambda:h.api()['topology_revision']>=body['expected_revision']+1)
            return body
        def done(body, delivery):
            def poll():
                op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}')
                require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and op['delivery']['state']==delivery else None
            return wait(poll)
        removed=done(request('remove_destination','primary'),'not_written')
        raps=[]
        for _ in range(2):
            op=done(request('request_random_access'),'not_written')
            require(op['retired'] and op['delivery']['inflight_frame_leases']==0 and op['delivery']['first_written'] is None,op)
            raps.append(op)
        mirror=request('add_destination','mirror',dict(output=str(directory/'mirror.mp4'),output_format='mp4'))
        # A later real writer still receives an explicitly requested IDR and
        # confirms actual delivery; empty-target cleanup cannot poison reuse.
        delivered=done(request('request_random_access'),'first_written');attached=done(mirror,'first_written')
        require(delivered['delivery']['inflight_frame_leases']==0,delivered)
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,final)
        destination=next(d for d in final['outputs'][0]['destinations'] if d['destination_id']=='mirror')
        expected=frames-attached['applied_checkpoint']['frame_id']+1
        require(destination['packets_written']==expected and len(decoded_hashes(directory/'mirror.mp4'))==expected,('mirror suffix after empty destination RAP',expected,destination))
        report.update(passed=True,backend=backend,removed=removed,empty_raps=raps,delivered=delivered,mirror=attached,expected_frames=expected,final=final)
    finally:
        h.close();(directory/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    run=Path(tempfile.mkdtemp(prefix='rendition-destinations-',dir=ROOT/'out'));sample=run/'source.ts';frames=300
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-f','mpegts',sample])
    h=Harness(run);report={};stop_sink=threading.Event();accepted_sink=threading.Event();connections=[]
    sink=socket.socket();sink.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024);sink.bind(('127.0.0.1',0));sink.listen(1);sink.settimeout(1)
    def stall_sink():
        while not stop_sink.is_set():
            try:
                connection,_=sink.accept();connections.append(connection);accepted_sink.set();stop_sink.wait(30);connection.close();return
            except socket.timeout:continue
            except OSError:return
    sink_thread=threading.Thread(target=stall_sink,daemon=True);sink_thread.start()
    try:
        h.start();config=dict(input=str(sample),outputs=[dict(id='main',output=str(run/'primary.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',audio_copy=False)],rendition_backend='process',pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'})
        h.api(body=config,expected=202)
        state=wait(lambda:(s if (s:=h.api()).get('packets_out',0)>=8 else None));job=state['job_id'];pid=state['outputs'][0]['rendition_pid'];generation=state['outputs'][0]['rendition_generation'];events=[]
        def request(action,id=None,spec=None):
            current=h.api();body=dict(operation_id=uuid.uuid4().hex,action=action,expected_revision=current['topology_revision'],expected_generation=generation,target=dict(component='destination' if id else 'rendition',output_id='main'))
            if id:body['target']['destination_id']=id
            if spec:body['spec']=spec
            accepted=h.api(f'/v1/jobs/{job}/updates',body,202);events.append(dict(request=body,accepted=accepted));return body
        def done(body,written=False):
            def poll():
                op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}')
                require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and (not written or op['delivery']['state']=='first_written') else None
            op=wait(poll);events[-1]['result']=op;return op
        mirror=request('add_destination','mirror',dict(output=str(run/'mirror.mp4'),output_format='mp4'))
        # Explicit RAP request is separate from the reuse command and ordinary
        # I-frame policy. A real IDR must be observed before the mirror starts.
        wait(lambda:h.api()['topology_revision']>=mirror['expected_revision']+1)
        rap=request('request_random_access');rap_done=done(rap,True);mirror_done=done(mirror,True)
        state=h.api();require(state['outputs'][0]['rendition_pid']==pid,'reuse restarted the encoder')
        require(mirror_done['delivery']['first_written']['frame_id']>=mirror_done['applied_checkpoint']['frame_id'],mirror_done)
        detail=h.api(f'/v1/jobs/{job}/renditions/main');require({x['destination_id'] for x in detail['destinations']}=={'primary','mirror'},detail)
        replay=h.api(f'/v1/jobs/{job}/updates',mirror,(200,202));require(replay['operation_id']==mirror['operation_id'] and replay['state']=='applied',replay)
        wait(lambda:(d if (d:=h.api(f'/v1/jobs/{job}/renditions/main'))['packets_out']>=40 else None))
        remove=request('remove_destination','mirror');removed=done(remove)
        # Retire/reuse a logical destination only after its old writer joined.
        second=request('add_destination','mirror',dict(output=str(run/'mirror2.mp4'),output_format='mp4'));second_done=done(second,True)
        # One failed destination must not poison primary or the surviving mirror.
        bad=request('add_destination','bad',dict(output=str(run/'missing'/'bad.mp4'),output_format='mp4'))
        bad_done=wait(lambda:(op if (op:=h.api(f'/v1/jobs/{job}/operations/{bad["operation_id"]}'))['state']=='failed' else None));events[-1]['result']=bad_done
        slow=request('add_destination','slow',dict(output=f'tcp://127.0.0.1:{sink.getsockname()[1]}?send_buffer_size=4096&tcp_nodelay=1',output_format='mpegts'))
        require(accepted_sink.wait(5),'slow transport was never connected')
        def failed_sink():
            detail=h.api(f'/v1/jobs/{job}/renditions/main');entry=next((d for d in detail.get('destinations',[]) if d['destination_id']=='slow'),None)
            return detail if entry and entry['state']=='failed' else None
        report['slow_failed']=wait(failed_sink,10)
        require(h.api()['outputs'][0]['rendition_pid']==pid,'slow destination killed the encoder')
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,final)
        require(final['outputs'][0]['packets_out']==frames,'primary dropped source frames')
        primary_hashes=decoded_hashes(run/'primary.mp4');require(len(primary_hashes)==frames,'primary decode count')
        # Fixed expected suffix, not inferred from actual output count.
        expected=frames-second_done['applied_checkpoint']['frame_id']+1
        mirror_hashes=decoded_hashes(run/'mirror2.mp4');require(len(mirror_hashes)==expected,('mirror suffix count',expected))
        require(mirror_hashes==primary_hashes[second_done['applied_checkpoint']['frame_id']-1:],'reused encoder decoded pixels diverged')
        rows=frame_rows(final['outputs'][0]['log_path']);require([r['frame_id'] for r in rows]==list(range(1,frames+1)),'primary sequence')
        # Independent parser/decoder must reject any broken startup/references.
        for name in ['mirror.mp4','mirror2.mp4']:
            command([ORACLE_FFMPEG,'-v','error','-xerror','-i',run/name,'-f','null','-'])
        report['survivor']=survivor_case(run/'survivor',sample,frames)
        report['empty_destinations']=[empty_destination_case(run/f'empty-{backend}',sample,frames,backend) for backend in ('process','thread')]
        report.update(passed=True,events=events,final=final,rap=rap_done,mirror=mirror_done,mirror_second_expected=expected,first_removed=removed)
    finally:
        h.close();stop_sink.set();sink.close();sink_thread.join(timeout=3);(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
