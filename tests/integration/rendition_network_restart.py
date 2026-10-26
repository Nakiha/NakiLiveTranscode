#!/usr/bin/env python3
"""Repeated real TCP receiver reconnects after rendition SIGKILL, with AAC."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0,str(_pathlib.Path(__file__).resolve().parents[1]))
import json
from pathlib import Path
import signal
import socket
import tempfile
import threading
import uuid
from integration.rendition_process_integration import ROOT, BIN, SDK, ORACLE_FFMPEG, Harness, command, wait, require, frame_rows, decoded_hashes
from integration.rendition_recovery import send_signal
from integration import sei_integration


def main():
    run=Path(tempfile.mkdtemp(prefix='rendition-reconnect-',dir=ROOT/'out'));source=run/'source.mp4';frames=400;cycles=3;report={};stop=threading.Event();closed=[];errors=[]
    base=run/'video.mp4';sei=run/'sei.mp4'
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','7',base])
    command([BIN/'naki-sei-fixture',base,sei,'dynamic'])
    command([ORACLE_FFMPEG,'-v','error','-i',sei,'-f','lavfi','-i','sine=frequency=880:sample_rate=48000','-t',str(frames/25),'-map','0:v:0','-map','1:a:0','-c:v','copy','-c:a','aac','-ac','1',source])
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4);listener.settimeout(.2);endpoint=f'tcp://127.0.0.1:{listener.getsockname()[1]}?tcp_nodelay=1'
    def receive():
        index=0
        try:
            while not stop.is_set():
                try:connection,_=listener.accept()
                except socket.timeout:continue
                path=run/f'connection-{index}.flv';index+=1
                with connection,path.open('wb') as output:
                    connection.settimeout(.2)
                    while not stop.is_set():
                        try:data=connection.recv(65536)
                        except socket.timeout:continue
                        if not data:break
                        output.write(data)
                closed.append(path)
        except OSError as error:
            if not stop.is_set():errors.append(repr(error))
    receiver=threading.Thread(target=receive,daemon=True);receiver.start();h=Harness(run)
    try:
        h.start();outputs=[dict(id='healthy',output=str(run/'healthy.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',audio_copy=False),dict(id='network',output=endpoint,output_format='flv',width=160,height=96,encoder_preset='ultrafast',audio_copy=True,sei={'types':[100]})]
        h.api(body=dict(input=str(source),outputs=outputs,rendition_backend='process',pace=True,decoder_pump=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        state=wait(lambda:(s if len((s:=h.api()).get('outputs',[]))==2 and all(x.get('packets_out',0)>=15 for x in s['outputs']) else None));job=state['job_id'];root=state['supervisor']['worker_pid'];healthy_pid=next(x['rendition_pid'] for x in state['outputs'] if x['output_id']=='healthy');root_fds=len(list(Path(f'/proc/{root}/fd').iterdir()));events=[]
        for index in range(cycles):
            old=next(x for x in state['outputs'] if x['output_id']=='network');send_signal(old['rendition_pid'],signal.SIGKILL)
            def exited():
                s=h.api();return s if not next(x for x in s['outputs'] if x['output_id']=='network')['rendition_pid'] else None
            failed=wait(exited);wait(lambda:len(closed)==index+1)
            body=dict(operation_id=uuid.uuid4().hex,action='restart_rendition',target=dict(component='rendition',output_id='network'),expected_revision=failed['topology_revision'],expected_generation=old['rendition_generation']);h.api(f'/v1/jobs/{job}/updates',body,202)
            def delivered():
                op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
                return op if op['state']=='applied' and op['delivery']['state']=='first_written' else None
            operation=wait(delivered)
            def progressed():
                s=h.api();current=next(x for x in s['outputs'] if x['output_id']=='network')
                return s if current['packets_out']>=15 and current['rendition_pid'] else None
            state=wait(progressed);new=next(x for x in state['outputs'] if x['output_id']=='network')
            require(state['supervisor']['worker_pid']==root and next(x['rendition_pid'] for x in state['outputs'] if x['output_id']=='healthy')==healthy_pid,'network restart replaced healthy/shared owner')
            require(new['rendition_generation']>old['rendition_generation'] and new['actual_output']==endpoint,'network endpoint or holder generation contract')
            require(len(list(Path(f'/proc/{root}/fd').iterdir()))==root_fds,'repeated crash/restart leaked owner FD/pidfd')
            events.append(dict(operation=operation,generation=new['rendition_generation'],old_delivery='last write may be unacknowledged; connection is intentionally discontinuous'))
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30);wait(lambda:len(closed)==cycles+1)
        require(not errors and final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,(errors,final))
        expected=list(range(events[-1]['operation']['applied_checkpoint']['frame_id'],frames+1));path=closed[-1];rows=frame_rows(new['log_path']);require([x['frame_id'] for x in rows]==expected,'reconnected source frame sequence')
        require(len(decoded_hashes(path))==len(expected),'receiver decoded suffix count')
        original,_=sei_integration.messages(source,'h264');actual,nals=sei_integration.messages(path,'h264');require(nals and 5 in nals[0],'receiver new sequence lacks IDR')
        for row in rows:
            pts=round(__import__('fractions').Fraction(row['pts_us']*90000,1000000));require([x for x in original[pts] if x[0]==100]==[x for x in actual[pts] if x[0]==100],'receiver SEI loss/duplication')
        packets=json.loads(command([SDK/'ffprobe','-v','error','-show_packets','-show_entries','packet=codec_type,pts_time','-of','json',path]))['packets'];audio=[float(x['pts_time']) for x in packets if x['codec_type']=='audio'];video=[float(x['pts_time']) for x in packets if x['codec_type']=='video']
        require(audio and audio==sorted(audio) and abs(audio[0]-video[0])<=.1 and abs(audio[-1]-video[-1])<=.1,'reconnected AAC anchor/tail drift')
        for stream in closed:
            # A SIGKILL may cut the last FLV tag; only the confirmed prefix is
            # promised on old connections. Decode a fixed first three frames.
            command([ORACLE_FFMPEG,'-v','error','-xerror','-i',stream,'-frames:v','3','-f','null','-'])
        healthy=next(x for x in final['outputs'] if x['output_id']=='healthy');require(healthy['packets_out']==frames and len(decoded_hashes(run/'healthy.mp4'))==frames,'healthy output interrupted')
        report.update(passed=True,cycles=cycles,receiver_connections=len(closed),expected_last_suffix=len(expected),root_fd_baseline=root_fds,events=events,final=final)
    finally:
        h.close();stop.set();listener.close();receiver.join(timeout=3);(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
