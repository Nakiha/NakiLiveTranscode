#!/usr/bin/env python3
"""Real same-PID AAC/MP3 changes with continuous timestamps and a healthy sibling."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import collections
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import urllib.request
from support.update_server import Server,wait
from support.event_logs import read_events

ROOT=pathlib.Path(__file__).resolve().parents[2]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
RUN = None
PROBE=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT','/root/.cache/naki-live-transcode-n9'))/'install/bin/ffprobe'
RESULTS={}
def command(args):
    result=subprocess.run(list(map(str,args)),capture_output=True,timeout=40)
    assert result.returncode==0,(args,result.returncode,result.stderr[-3000:]);return result.stdout
def probe(path):
    data=json.loads(command([PROBE,'-v','error','-show_streams','-show_packets','-show_data','-of','json',path]))
    audio={s['index']:[] for s in data['streams'] if s['codec_type']=='audio'}
    for packet in data['packets']:
        if packet['stream_index'] not in audio:continue
        raw=b''.join(bytes.fromhex(line.split(':',1)[1].strip().split('  ',1)[0]) for line in packet['data'].splitlines() if ':' in line)
        profile=rate=channels=None;payload=raw
        if len(raw)>=7 and raw[0]==255 and raw[1]&0xf6==0xf0:
            profile=(raw[2]>>6)+1;rate=(raw[2]>>2)&15;channels=((raw[2]&1)<<2)|(raw[3]>>6)
            size=((raw[3]&3)<<11)|(raw[4]<<3)|(raw[5]>>5);header=7 if raw[1]&1 else 9
            assert size==len(raw),(path,size,len(raw));payload=raw[header:]
        elif len(raw)>=4 and raw[0]==255 and raw[1]&0xe0==0xe0:
            profile=(raw[1]>>3)&3;rate=(raw[2]>>2)&3;channels=1 if raw[3]>>6==3 else 2
        audio[packet['stream_index']].append({'hash':hashlib.sha256(payload).hexdigest(),'signature':(profile,rate,channels),'dts':int(packet['dts'])})
    for rows in audio.values():assert all(b['dts']>a['dts'] for a,b in zip(rows,rows[1:])),path
    return data,audio
def source(video,healthy,name,codec,first,second):
    paths=[]
    for index,(rate,channels,profile) in enumerate((first,second)):
        path=RUN/f'{name}-{index}.{codec}'
        args=['ffmpeg','-v','error','-f','lavfi','-i',f'sine=frequency={440+index*97}:sample_rate={rate}:duration=2',
            '-c:a','aac' if codec=='aac' else 'libmp3lame','-b:a','64k','-ac',channels,'-threads','1']
        if codec=='aac':args+=['-profile:a',profile,'-f','adts']
        else:args+=['-write_xing','0','-id3v2_version','0','-f','mp3']
        command([*args,path]);paths.append(path)
    combined=RUN/f'{name}-continuous.{codec}';combined.write_bytes(b''.join(p.read_bytes() for p in paths))
    output=RUN/f'{name}-same-pid.ts'
    command(['ffmpeg','-v','error','-i',video,'-i',combined,'-i',healthy,'-map','0:v','-map','1:a','-map','2:a','-c','copy',
        '-muxdelay','0','-muxpreload','0','-pes_payload_size','0','-f','mpegts',output])
    return output
def subsequence(rows,source):
    position=0
    for row in rows:
        while position<len(source) and source[position]['hash']!=row['hash']:position+=1
        assert position<len(source),'output contains payload outside accepted input prefix'
        position+=1
def execute(name,sample,reason=None,multiple=False,stable_he=False,transition_override=None,pump=False):
    reference,input_audio=probe(sample);streams=sorted(input_audio);changing=streams[0]
    transition=None
    if reason:
        initial=input_audio[changing][0]['signature']
        transition=transition_override if transition_override is not None else next(i for i,row in enumerate(input_audio[changing]) if row['signature']!=initial)
        assert transition>=20
        if transition_override is None:assert all(row['signature']!=initial for row in input_audio[changing][transition:])
        else:
            packets=[p for p in reference['packets'] if p['stream_index']==changing]
            assert any('New Extradata'==s.get('side_data_type') for s in packets[transition].get('side_data_list',[])),packets[transition]
    if stable_he:
        profile=next(s for s in reference['streams'] if s['index']==changing)['profile'];assert 'HE-AAC' in profile,profile
    server=Server(RUN, name)
    try:
        outputs=[{'id':f'output-{i}','output':str(RUN/f'{name}-out-{i}.ts'),'output_format':'mpegts','encoder_preset':'ultrafast'} for i in range(2)]
        spec={'input':str(sample),'frame_log':True,'encoder_preset':'ultrafast'}
        if pump:
            assert not multiple
            spec.update(decoder_pump=True,reader_queue_capacity=1,queue_capacity=1,
                        min_threads=2,max_threads=2,budget_slots=2)
        if multiple:spec['outputs']=outputs
        else:spec.update(output=str(RUN/f'{name}-out.ts'),output_format='mpegts')
        accepted=server.api('/v1/jobs',{'operation_id':'create-'+name,'spec':spec},202)
        final=wait(lambda:(s if (s:=server.api())['state'] in ('completed','completed_partial','failed') else None),30)
        assert final['state']=='completed' and final['supervisor']['exit_code']==0,final
        branches=final['outputs'] if multiple else [final];events=read_events(server.logs/'logs/events')
        for index,branch in enumerate(branches):
            assert branch['packets_out']==150 and branch['audio_tracks']==len(streams),branch
            output_id=f'output-{index}' if multiple else 'main'
            rows=[e for e in events if e.get('job_id')==accepted['job_id'] and e.get('output_id')==output_id]
            changes=[e for e in rows if e['event']=='audio_track_changed_unsupported']
            if reason:
                assert len(changes)==1 and changes[0]['reason']==reason and changes[0]['input_stream']==changing,changes
                assert branch['audio_unsupported']==len(input_audio[changing])-transition,(branch,transition)
            else:assert not changes and not branch['audio_unsupported'],(changes,branch)
            added=[e for e in rows if e['event']=='audio_track_added'];assert len(added)==len(streams),added
            destination=RUN/(f'{name}-out-{index}.ts' if multiple else f'{name}-out.ts');_,output_audio=probe(destination)
            for event in added:
                input_index=event['input_stream'];output_index=event['output_stream'];observed=output_audio[output_index]
                allowed=input_audio[input_index][:transition] if reason and input_index==changing else input_audio[input_index]
                assert observed and len(observed)<=len(allowed);subsequence(observed,allowed)
                if input_index!=changing:assert observed[-1]['hash']==allowed[-1]['hash'],'healthy sibling lost its tail'
            assert branch['audio_packets_out']+branch['audio_dropped']+branch['audio_unsupported']==sum(map(len,input_audio.values())),branch
            if multiple:assert not branch['queue_items'] and not branch['inflight_frames'],branch
        RESULTS[name]={'source':str(sample),'source_sha256':hashlib.sha256(sample.read_bytes()).hexdigest(),'transition_packet':transition,
            'continuous_input_packet_counts':{str(i):len(v) for i,v in input_audio.items()},'final':final}
    finally:server.close()
def he_fixture(video):
    expected='7e7c0390c950eda21775d1a193e05ee703257bed85602884e7d30a8b3c65be90'
    origin='https://fate-suite.ffmpeg.org/aac/al_sbr_cm_48_2.mp4';path=RUN/'fate-al_sbr_cm_48_2.mp4'
    supplied=os.environ.get('NAKI_HE_AAC_FIXTURE')
    if supplied:path.write_bytes(pathlib.Path(supplied).read_bytes())
    else:
        with urllib.request.urlopen(origin,timeout=20) as response:data=response.read(200000)
        path.write_bytes(data)
    assert path.stat().st_size==199678 and hashlib.sha256(path.read_bytes()).hexdigest()==expected
    (RUN/'HE-AAC-PROVENANCE.json').write_text(json.dumps({'url':origin,'sha256':expected,'bytes':199678},indent=2)+'\n')
    clipped=RUN/'stable-he-four-seconds.m4a'
    command(['ffmpeg','-v','error','-i',path,'-map','0:a','-c','copy','-t','4',clipped])
    output=RUN/'stable-he.ts'
    command(['ffmpeg','-v','error','-i',video,'-i',clipped,'-map','0:v','-map','1:a','-c','copy',
        '-muxdelay','0','-muxpreload','0','-pes_payload_size','0','-f','mpegts',output])
    return output
def run():
    global RUN
    RUN=pathlib.Path(tempfile.mkdtemp(prefix='audio-signature-',dir=ROOT/'out'))
    video=RUN/'video.mp4';healthy=RUN/'healthy.aac'
    command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25:duration=6','-c:v','libx264','-threads','2','-bf','3',video])
    command(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=880:sample_rate=48000:duration=4','-c:a','aac','-ac','2','-b:a','64k','-threads','1','-f','adts',healthy])
    cases=[('aac-rate','aac',(48000,2,'aac_low'),(44100,2,'aac_low'),'core_sample_rate_changed',False),
        ('aac-layout','aac',(48000,2,'aac_low'),(48000,1,'aac_low'),'channel_layout_changed',True),
        ('aac-profile','aac',(48000,2,'aac_low'),(48000,2,'aac_main'),'audio_object_type_changed',False),
        ('mp3-rate','mp3',(48000,2,None),(44100,2,None),'core_sample_rate_changed',False),
        ('mp3-layout','mp3',(48000,2,None),(48000,1,None),'channel_layout_changed',False)]
    for name,codec,first,second,reason,multiple in cases:
        sample=source(video,healthy,name,codec,first,second)
        execute(name,sample,reason,multiple)
        execute(name+'-pump',sample,reason,pump=True)
    for name,reason in (('aac-rate','core_sample_rate_changed'),('aac-profile','audio_object_type_changed')):
        path=RUN/f'{name}-asc.flv'
        metadata=json.loads(command([BIN/'naki-audio-change-fixture',video,RUN/f'{name}-0.aac',RUN/f'{name}-1.aac',path]))
        (RUN/f'{name}-asc-fixture.json').write_text(json.dumps(metadata,indent=2)+'\n')
        execute(name+'-asc',path,reason,transition_override=metadata['first_phase_packets'])
        execute(name+'-asc-pump',path,reason,transition_override=metadata['first_phase_packets'],pump=True)
    he=he_fixture(video);execute('stable-he',he,stable_he=True);execute('stable-he-multiple',he,multiple=True,stable_he=True)
    execute('stable-he-pump',he,stable_he=True,pump=True)
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','cases':RESULTS},indent=2)+'\n')
    print('PASS real continuous-PTS same-PID AAC/MP3 changes and stable official HE-AAC:',RUN,flush=True)
if __name__=='__main__':run()
