#!/usr/bin/env python3
"""Real rational cadence/GOP, ignored known-rate fallback and unknown-rate injection."""
import fractions
import json
import math
import os
import pathlib
import shutil
import subprocess
import tempfile
from update_api_integration import Server,wait

ROOT=pathlib.Path(__file__).resolve().parents[1];BIN=ROOT/'out/bin'
RUN=pathlib.Path(tempfile.mkdtemp(prefix='video-rate-',dir=ROOT/'out'))
PROBE=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT','/root/.cache/naki-live-transcode-n9'))/'install/bin/ffprobe'
RESULTS={}
def command(args):
    result=subprocess.run(list(map(str,args)),capture_output=True,timeout=35)
    assert result.returncode==0,(args,result.returncode,result.stderr[-2500:]);return result.stdout
def frames(path):
    data=json.loads(command([PROBE,'-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=pts_time,key_frame,pict_type','-of','json',path]))
    return data['frames']
def execute(server,name,source,fallback,expected,unknown=False,multiple=False):
    reference=frames(source);count=len(reference);output=RUN/f'{name}.ts'
    spec={'input':str(source),'fps':fallback,'encoder_preset':'ultrafast','frame_log':True}
    outputs=[{'id':f'branch-{i}','output':str(RUN/f'{name}-{i}.ts'),'output_format':'mpegts'} for i in range(2)]
    if multiple:spec['outputs']=outputs
    else:spec.update(output=str(output),output_format='mpegts')
    server.api('/v1/jobs',{'operation_id':'create-'+name,'spec':spec},202)
    final=wait(lambda:(s if (s:=server.api())['state'] in ('completed','completed_partial','failed') else None),25)
    assert final['state']=='completed' and final['supervisor']['exit_code']==0,final
    branches=final['outputs'] if multiple else [final];results=[]
    nominal_gop=max(1,math.floor(expected*2+fractions.Fraction(1,2)));work=max(1,math.floor(1000000/expected+fractions.Fraction(1,2)))
    for index,branch in enumerate(branches):
        rate=branch['video_rate'];assert fractions.Fraction(rate['num'],rate['den'])==expected,rate
        assert rate['source']==('config_fallback' if unknown else 'av_guess_frame_rate') and rate['fallback_fps']==fallback,rate
        assert rate['nominal_gop_frames']==nominal_gop and rate['nominal_frame_work_us']==work,rate
        assert rate['rc_nominal_rate']=={'num':rate['num'],'den':rate['den']}
        assert branch['packets_out']==count and branch['frames_filtered']==count,branch
        decoded=frames(RUN/f'{name}-{index}.ts' if multiple else output);assert len(decoded)==count
        actual=[float(f['pts_time']) for f in decoded];original=[float(f['pts_time']) for f in reference]
        assert all(abs(a-b)<.00002 for a,b in zip(actual,original)),(name,actual[:5],original[:5])
        keys=[i for i,f in enumerate(decoded) if f['key_frame']]
        assert keys==list(range(0,count,nominal_gop)),(name,keys,nominal_gop)
        if multiple:assert not branch['queue_items'] and not branch['inflight_frames']
        results.append({'rate':rate,'keys':keys,'pts':actual})
    RESULTS[name]={'final':final,'decoded':results};return results
def run():
    server=Server('video-rate-known');sources={}
    try:
        for rate in ('24','25','30','50','60','30000/1001'):
            rational=fractions.Fraction(rate);name=rate.replace('/','-');source=RUN/f'input-{name}.mp4';sources[rate]=source
            count=math.ceil(rational*fractions.Fraction(21,5))
            command(['ffmpeg','-v','error','-f','lavfi','-i',f'color=c=blue:size=160x96:rate={rate}','-frames:v',count,
                '-c:v','libx264','-threads','2','-g','1000','-sc_threshold','0','-bf','3',source])
            first=execute(server,f'{name}-fallback7',source,7,rational)
            second=execute(server,f'{name}-fallback173',source,173,rational)
            assert first[0]['keys']==second[0]['keys'] and first[0]['pts']==second[0]['pts']
        execute(server,'fractional-multiple',sources['30000/1001'],19,fractions.Fraction(30000,1001),multiple=True)
        gap=RUN/'offset-gap.mp4'
        command(['ffmpeg','-v','error','-f','lavfi','-i','color=c=blue:size=160x96:rate=25','-frames:v','105',
            '-vf',r'settb=1/90000,setpts=450000+N*3600+gte(N\,20)*630000','-fps_mode','passthrough',
            '-c:v','libx264','-threads','2','-g','1000','-sc_threshold','0','-bf','3','-video_track_timescale','90000',gap])
        # The bounded startup probe reports this container's 105/11.2 rate;
        # nominal hints must follow that result without compressing the gap.
        decoded=execute(server,'vfr-offset-gap',gap,101,fractions.Fraction(75,8),multiple=True)
        assert abs(decoded[0]['pts'][0]-5)<.00002 and max(b-a for a,b in zip(decoded[0]['pts'],decoded[0]['pts'][1:]))>7
    finally:server.close()
    binaries=RUN/'unknown-bin';binaries.mkdir();hooks=RUN/'hooks';hooks.mkdir()
    for exe in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/exe,binaries/exe)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    server=Server('video-rate-unknown',binaries/'naki-live',{'NAKI_TEST_FRAME_RATE_UNKNOWN':'1','NAKI_TEST_MEDIA_HOOK_DIR':str(hooks)})
    try:
        first=execute(server,'injected-unknown15',sources['25'],15,fractions.Fraction(15),unknown=True)
        second=execute(server,'injected-unknown30',sources['25'],30,fractions.Fraction(30),unknown=True)
        assert first[0]['pts']==second[0]['pts'] and first[0]['keys']!=second[0]['keys']
    finally:server.close()
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','unknown_rate_evidence':'test-only av_guess_frame_rate returns unknown; real encoded source/PTS unchanged','cases':RESULTS},indent=2)+'\n')
    print('PASS rational cadence/GOP/work, known-rate fallback invariance, VFR offset/gap and injected unknown-rate path:',RUN,flush=True)
if __name__=='__main__':run()
