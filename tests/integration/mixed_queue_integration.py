#!/usr/bin/env python3
"""Default TS packaging,16 AAC tracks,2/4 owners and a bounded owner pause."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from support.event_logs import read_events

ROOT=pathlib.Path(__file__).resolve().parents[2]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
RUN = None
PROBE=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT','/root/.cache/naki-live-transcode-n9'))/'install/bin/ffprobe'

def command(args):
    r=subprocess.run(list(map(str,args)),capture_output=True,timeout=35)
    assert r.returncode==0,(args,r.returncode,r.stderr[-2000:]);return r.stdout

def packets(path):
    data=json.loads(command([PROBE,'-v','error','-show_packets','-show_streams','-show_data','-show_entries',
        'stream=index,codec_type:packet=stream_index,data','-of','json',path]))
    audio={s['index']:[] for s in data['streams'] if s['codec_type']=='audio'};longest=burst=0
    for packet in data['packets']:
        index=packet['stream_index']
        if index not in audio:burst=0;continue
        burst+=1;longest=max(longest,burst)
        raw=b''.join(bytes.fromhex(line.split(':',1)[1].strip().split('  ',1)[0]) for line in packet['data'].splitlines() if ':' in line)
        position=0
        while position<len(raw):
            assert raw[position]==255 and raw[position+1]&0xf6==0xf0
            size=((raw[position+3]&3)<<11)|(raw[position+4]<<3)|(raw[position+5]>>5);header=7 if raw[position+1]&1 else 9
            assert size>=header and position+size<=len(raw)
            audio[index].append(raw[position+header:position+size]);position+=size
    return audio,longest

def wait(predicate,timeout=20):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        result=predicate()
        if result:return result
        time.sleep(.025)
    raise AssertionError('condition timed out')

def run():
    global RUN
    RUN=pathlib.Path(tempfile.mkdtemp(prefix='mixed-queue-',dir=ROOT/'out'))
    sample=RUN/'sixteen-aac.ts'
    args=['ffmpeg','-v','error','-filter_threads','1','-filter_complex_threads','1','-f','lavfi','-i','testsrc2=size=160x96:rate=25:duration=3.2',
        '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=3.2','-map','0:v']
    for _ in range(16):args+=['-map','1:a']
    command([*args,'-c:v','libx264','-preset','ultrafast','-threads:v','2','-g','25','-bf','0','-c:a','aac','-b:a','32k','-ac','2','-threads:a','1','-f','mpegts',sample])
    original,burst=packets(sample);assert len(original)==16 and burst>=16,(len(original),burst)
    smooth=RUN/'smooth-one-aac.ts'
    command(['ffmpeg','-v','error','-i',sample,'-map','0:v:0','-map','0:a:0','-c','copy','-muxdelay','0','-muxpreload','0','-pes_payload_size','0','-f','mpegts',smooth])
    smooth_audio,smooth_burst=packets(smooth);assert len(smooth_audio)==1 and smooth_burst<=4,(len(smooth_audio),smooth_burst)
    binaries=RUN/'bin';binaries.mkdir()
    for name in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/name,binaries/name)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    hooks=RUN/'hooks';hooks.mkdir()
    state=tempfile.TemporaryDirectory(prefix='mix-')
    with socket.socket() as listener:listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    def api(path='/v1/job',body=None,code=200):
        request=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        try:response=urllib.request.urlopen(request,timeout=4)
        except urllib.error.HTTPError as e:response=e
        with response:
            data=json.load(response);assert response.status==code,(response.status,data);return data
    log=(RUN/'controller.stdout').open('wb');error=(RUN/'controller.stderr').open('wb')
    environment=dict(os.environ,NAKI_TEST_MEDIA_HOOK_DIR=str(hooks),NAKI_TEST_MEDIA_HOOK_MODE='pause_video',
        NAKI_TEST_MEDIA_HOOK_PAUSE_OUTPUT='paused',NAKI_TEST_HOOK_OUTPUT_PATHS='1')
    server=subprocess.Popen([str(binaries/'naki-live'),'--port',str(port),'--run-dir',str(RUN/'logs'),'--state-dir',state.name],stdout=log,stderr=error,env=environment)
    results=[]
    try:
        def online():
            assert server.poll() is None
            try:return api('/health')
            except OSError:return None
        wait(online)
        for name,outputs,source,reference in (('default-2',2,sample,original),('default-4',4,sample,original),('smooth-2',2,smooth,smooth_audio)):
            tracks=len(reference)
            specs=[{'id':'paused' if index==0 else f'fast-{index}','output':str(RUN/f'{name}-{index}.ts'),'output_format':'mpegts',
                    'encoder_preset':'ultrafast','video_items':16,'audio_packets':256} for index in range(outputs)]
            accepted=api('/v1/jobs',{'operation_id':f'create-{name}','spec':{'input':str(source),'outputs':specs,'pace':True}},202)
            final=wait(lambda:(s if (s:=api())['state'] in ('completed','completed_partial','failed') else None));assert final['state']=='completed',final
            assert final['supervisor']['exit_code']==0,final
            paused=next(b for b in final['outputs'] if b['output_id']=='paused')
            report=json.loads((hooks/f'{accepted["job_id"]}-paused.json').read_text())
            assert report['video']==80 and paused['first_video_mux_us']-report['first_encoded_us']>=39000,(paused,report)
            events=read_events(RUN/'logs/events')
            for spec in specs:
                branch=next(b for b in final['outputs'] if b['output_id']==spec['id'])
                assert branch['packets_out']==80 and branch['audio_tracks']==tracks and not branch['audio_unsupported'],branch
                assert not branch['queue_items'] and not branch['queue_video_items'] and not branch['queue_audio_packets'] and not branch['inflight_frames'],branch
                assert branch['queue_video_high']<=16 and branch['queue_audio_high']<=256 and branch['queue_high_items']<=272
                assert branch['queue_limits']['video_items']==16 and branch['queue_limits']['audio_packets']==256
                mapping=[r for r in events if r.get('event')=='audio_track_added' and r.get('job_id')==accepted['job_id'] and r.get('output_id')==spec['id']]
                assert len(mapping)==tracks,(spec['id'],mapping)
                actual,_=packets(spec['output']);assert len(actual)==tracks
                for track in mapping:
                    payloads=iter(reference[track['input_stream']])
                    for payload in actual[track['output_stream']]:assert any(candidate==payload for candidate in payloads)
                copied=sum(map(len,actual.values()));assert copied==branch['audio_packets_out'],(copied,branch)
                assert copied+branch['audio_dropped']==sum(map(len,reference.values())),branch
            results.append(final)
    finally:
        server.terminate();server.wait(timeout=8);log.close();error.close();state.cleanup()
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','tracks':16,'longest_audio_burst':burst,'smooth_audio_burst':smooth_burst,'owner_pause_us':40000,'cases':results},indent=2)+'\n')
    print('PASS default TS16AAC tracks,2/4 owners,40ms owner pause and retained payload/accounting:',RUN,flush=True)

if __name__=='__main__':run()
