#!/usr/bin/env python3
"""Dynamic audio/SEI subscription barrier with real B-frame decode reorder."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import signal
from integration.rendition_recovery import send_signal
from pathlib import Path
import tempfile
import uuid
from integration.rendition_process_integration import ROOT, BIN, ORACLE_FFMPEG, SDK, Harness, command, wait, require, frame_rows, decoded_hashes
from integration import sei_integration


def restart_case(directory, source, frames):
    directory.mkdir();h=Harness(directory);report={}
    try:
        h.start();specs=[dict(id=name,output=str(directory/(name+'.flv')),output_format='flv',width=160,height=96,encoder_preset='ultrafast',audio_copy=True,sei={'types':[100]}) for name in ['healthy','restart']]
        h.api(body=dict(input=str(source),outputs=specs,rendition_backend='process',pace=True,decoder_pump=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        state=wait(lambda:(s if len((s:=h.api()).get('outputs',[]))==2 and all(x.get('packets_out',0)>=30 for x in s['outputs']) else None));job=state['job_id'];old=next(x for x in state['outputs'] if x['output_id']=='restart');healthy_pid=next(x['rendition_pid'] for x in state['outputs'] if x['output_id']=='healthy');root=state['supervisor']['worker_pid']
        send_signal(old['rendition_pid'],signal.SIGKILL)
        def exited():
            s=h.api();return s if not next(x for x in s['outputs'] if x['output_id']=='restart')['rendition_pid'] else None
        wait(exited)
        body=dict(operation_id=uuid.uuid4().hex,action='restart_rendition',expected_revision=h.api()['topology_revision'],expected_generation=old['rendition_generation'],target=dict(component='rendition',output_id='restart'))
        h.api(f'/v1/jobs/{job}/updates',body,202)
        def delivered():
            op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
            return op if op['state']=='applied' and op['delivery']['state']=='first_written' else None
        operation=wait(delivered);current=h.api();new=next(x for x in current['outputs'] if x['output_id']=='restart')
        require(current['supervisor']['worker_pid']==root and next(x['rendition_pid'] for x in current['outputs'] if x['output_id']=='healthy')==healthy_pid,'A/V restart replaced shared input or healthy encoder')
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,final)
        first=operation['applied_checkpoint']['frame_id'];expected=list(range(first,frames+1));rows=frame_rows(new['log_path']);require([r['frame_id'] for r in rows]==expected,'restarted A/V source frame sequence')
        actual=new['actual_output'];require(actual!=specs[1]['output'],'restart overwrote the previous output generation')
        require(len(decoded_hashes(actual))==len(expected),'independently decoded restarted sequence')
        packets=json.loads(command([SDK/'ffprobe','-v','error','-show_packets','-show_entries','packet=codec_type,pts_time,flags','-of','json',actual]))['packets'];audio=[float(x['pts_time']) for x in packets if x['codec_type']=='audio'];video=[x for x in packets if x['codec_type']=='video']
        require(video and 'K' in video[0]['flags'],'restarted sequence lacks a decoder-validated random access start')
        require(audio and audio==sorted(audio) and abs(audio[0]-float(video[0]['pts_time']))<=.1 and abs(audio[-1]-float(video[-1]['pts_time']))<=.1,'restarted A/V anchor or drift')
        parsed=directory/'restarted.flv';parsed.write_bytes(Path(actual).read_bytes())
        original,_=sei_integration.messages(source,'h264');messages,nals=sei_integration.messages(parsed,'h264');require(nals and 5 in nals[0],'restart first access unit is not an AVC IDR')
        for row in rows:
            pts=round(__import__('fractions').Fraction(row['pts_us']*90000,1000000));require([x for x in original[pts] if x[0]==100]==[x for x in messages[pts] if x[0]==100],'restarted SEI mapping')
        healthy=next(x for x in final['outputs'] if x['output_id']=='healthy');require(healthy['packets_out']==frames and len(decoded_hashes(specs[0]['output']))==frames,'healthy A/V branch interrupted')
        report.update(passed=True,operation=operation,expected_restart_frames=len(expected),actual_output=actual,first_audio_s=audio[0],first_video_s=float(video[0]['pts_time']),final=final,precrash_delivery='acknowledged prefix only; last write before SIGKILL may lack ACK')
    finally:
        h.close();(directory/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def mp3_case(directory):
    directory.mkdir();source=directory/'source.ts';frames=75;h=Harness(directory);report={}
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-f','lavfi','-i','sine=frequency=660:sample_rate=44100','-t','3','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-bf','3','-c:a','libmp3lame','-ac','1','-muxdelay','0','-pes_payload_size','0','-f','mpegts',source])
    oracle=json.loads(command([SDK/'ffprobe','-v','error','-select_streams','a:0','-show_packets','-show_entries','packet=pts_time','-of','json',source]));require(len(oracle['packets'])==116,'MP3 fixture must contain the fixed 116 coded audio frames')
    try:
        h.start();specs=[dict(id=format,output=str(directory/('out.'+('ts' if format=='mpegts' else format))),output_format=format,width=160,height=96,encoder_preset='ultrafast',audio_copy=True) for format in ['mpegts','flv']]
        h.api(body=dict(input=str(source),outputs=specs,rendition_backend='process',pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'}),expected=202)
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),20)
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,final)
        observations=[]
        for spec in specs:
            state=next(x for x in final['outputs'] if x['output_id']==spec['id']);require(state['packets_out']==frames and state['audio_packets_out']>0 and state['audio_unsupported']==0 and state['audio_packets_out']+state['audio_dropped']==116,state)
            require(len(decoded_hashes(spec['output']))==frames,'MP3 copy disturbed video')
            data=json.loads(command([SDK/'ffprobe','-v','error','-show_streams','-show_packets','-show_entries','stream=codec_type,codec_name,sample_rate,channels:packet=codec_type,pts_time','-of','json',spec['output']]))
            audio=next(x for x in data['streams'] if x['codec_type']=='audio');require(audio['codec_name']=='mp3' and int(audio['sample_rate'])==44100 and audio['channels']==1,audio)
            ap=[float(x['pts_time']) for x in data['packets'] if x['codec_type']=='audio'];vp=[float(x['pts_time']) for x in data['packets'] if x['codec_type']=='video']
            require(ap and all(b>a for a,b in zip(ap,ap[1:])) and abs(ap[0]-vp[0])<=.1 and abs(ap[-1]-vp[-1])<=.1,'MP3 copy clock/order/tail drift')
            command([ORACLE_FFMPEG,'-v','error','-xerror','-i',spec['output'],'-map','0:a:0','-f','null','-'])
            observations.append(dict(format=spec['output_format'],audio=audio,expected_audio_frames=116,audio_written=state['audio_packets_out'],audio_dropped_late=state['audio_dropped'],first_audio_s=ap[0],first_video_s=vp[0],last_audio_s=ap[-1],last_video_s=vp[-1]))
        report.update(passed=True,expected_video_frames=frames,outputs=observations,final=final)
    finally:
        h.close();(directory/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    run=Path(tempfile.mkdtemp(prefix='rendition-audio-sei-',dir=ROOT/'out'));frames=300;raw=run/'base.mp4';video=run/'sei.mp4';source=run/'source.mp4'
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','7',raw])
    command([BIN/'naki-sei-fixture',raw,video,'dynamic'])
    command([ORACLE_FFMPEG,'-v','error','-i',video,'-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t',str(frames/25),'-map','0:v:0','-map','1:a:0','-c:v','copy','-c:a','aac','-ac','1',source])
    h=Harness(run);report={}
    try:
        h.start();base=dict(id='base',output=str(run/'base.ts'),output_format='mpegts',width=160,height=96,encoder_preset='ultrafast',audio_copy=False)
        cfg=dict(input=str(source),outputs=[base],rendition_backend='process',pace=True,decoder_pump=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'})
        h.api(body=cfg,expected=202);state=wait(lambda:(s if (s:=h.api()).get('packets_out',0)>=8 else None));job=state['job_id'];pid=state['outputs'][0]['rendition_pid']
        extra=dict(base,id='extra',output=str(run/'extra.flv'),output_format='flv',audio_copy=True,output_fps=5,sei={'types':[100]})
        body=dict(operation_id=uuid.uuid4().hex,action='add_rendition',target=dict(component='rendition',output_id='extra'),expected_revision=state['topology_revision'],spec=extra)
        h.api(f'/v1/jobs/{job}/updates',body,202)
        def attached():
            op=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}');require(op['state'] not in ('failed','cancelled'),op)
            return op if op['state']=='applied' and op['delivery']['state']=='first_written' else None
        added=wait(attached)
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
        (run/'final.json').write_text(json.dumps(final,indent=2)+'\n')
        require(final['state']=='completed' and final['frames_decoded']==frames and final['shared_pool_occupied_slots']==0,(final['state'],final.get('error')))
        good=next(b for b in final['outputs'] if b['output_id']=='base');new=next(b for b in final['outputs'] if b['output_id']=='extra')
        rows=frame_rows(new['log_path']);expected=list(range(added['applied_checkpoint']['frame_id'],frames+1))
        require([r['frame_id'] for r in rows]==expected,'dynamic B/SEI frame identity order')
        require(len(decoded_hashes(extra['output']))==len(expected),'independent hot suffix frame count')
        require(good['packets_out']==frames,'subscription change disturbed existing branch')
        original,_=sei_integration.messages(source,'h264');actual,_=sei_integration.messages(Path(extra['output']),'h264')
        for row in rows:
            pts=round(__import__('fractions').Fraction(row['pts_us']*90000,1000000));want=[x for x in original[pts] if x[0]==100];got=[x for x in actual[pts] if x[0]==100]
            require(want==got and len(want)==2,('SEI epoch loss/duplication',pts,want,got))
        packets=json.loads(command([SDK/'ffprobe','-v','error','-show_packets','-show_entries','packet=codec_type,pts_time','-of','json',extra['output']]))['packets']
        audios=[float(x['pts_time']) for x in packets if x['codec_type']=='audio'];videos=[float(x['pts_time']) for x in packets if x['codec_type']=='video']
        require(audios and audios==sorted(audios),'missing or nonmonotonic audio')
        require(abs(audios[0]-videos[0])<=.10,('hot audio starts too far from first video',audios[0],videos[0]))
        require(abs(audios[-1]-videos[-1])<=.10,('audio/video tail drift',audios[-1],videos[-1]))
        command([ORACLE_FFMPEG,'-v','error','-xerror','-i',extra['output'],'-f','null','-'])
        report['restart']=restart_case(run/'restart',source,frames)
        report['mp3']=mp3_case(run/'mp3')
        report.update(passed=True,final=final,operation=added,expected_hot_frames=len(expected),first_audio_s=audios[0],first_video_s=videos[0],last_audio_s=audios[-1],last_video_s=videos[-1],base_pid=pid)
    finally:
        h.close();(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True)))


if __name__=='__main__':main()
