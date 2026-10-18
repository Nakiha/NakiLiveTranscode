#!/usr/bin/env python3
"""Extract actual muxed bytes independently; verify payload/order/PTS and protection."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import fractions
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import hashlib
from support.update_server import Server, wait
from support.images import png

ROOT=pathlib.Path(__file__).resolve().parents[2]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
RUN = None
PROBE=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT','/root/.cache/naki-live-transcode-n9'))/'install/bin/ffprobe'
RESULTS={}
def source_hashes():
    paths=list((ROOT/'app').rglob('*.[ch]'))+[ROOT/'CMakeLists.txt']+[p for p in (ROOT/'tests').rglob('sei*') if p.is_file() and p.suffix in ('.c','.h','.py')]
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}

def command(args,timeout=45):
    p=subprocess.run(list(map(str,args)),capture_output=True,timeout=timeout)
    assert p.returncode==0,(args,p.returncode,p.stderr[-3500:]);return p.stdout

def packets(path):
    data=json.loads(command([PROBE,'-v','error','-select_streams','v:0','-show_packets','-show_data',
        '-show_entries','packet=pts_time,data','-of','json',path]))
    return data['packets']

def messages(path,codec):
    result={};nals_per_packet=[]
    for p in packets(path):
        data=bytes.fromhex(''.join(line.split(':',1)[1].strip().split('  ',1)[0].replace(' ','')
            for line in p['data'].splitlines() if ':' in line))
        # Fixture MP4 uses four-byte NAL lengths. A length such as 0x0000016e
        # can resemble an Annex B start code, so never sniff those bytes.
        if pathlib.Path(path).suffix not in ('.mp4','.flv'):
            nals=[x for x in re.split(b'\x00\x00(?:\x00)?\x01',data) if x]
        else:
            nals=[];pos=0
            while pos<len(data):
                size=int.from_bytes(data[pos:pos+4],'big');pos+=4
                assert 0<size<=len(data)-pos
                nals.append(data[pos:pos+size]);pos+=size
        rows=[];types=[];private_tids=[];vcl_tid=None
        for nal in nals:
            typ=nal[0]&31 if codec=='h264' else (nal[0]>>1)&63 if codec=='hevc' else (nal[1]>>3)&31
            types.append(typ)
            if codec=='vvc' and typ<=11 and vcl_tid is None:vcl_tid=nal[1]&7
            if typ not in ({6} if codec=='h264' else {39,40} if codec=='hevc' else {23,24}):continue
            rbsp=nal[1 if codec=='h264' else 2:].rstrip(b'\0').replace(b'\0\0\3',b'\0\0')
            assert rbsp[-1:]==b'\x80',(path,typ,rbsp[-20:])
            pos=0
            while pos<len(rbsp)-1:
                typ=0
                while rbsp[pos]==255:typ+=255;pos+=1
                typ+=rbsp[pos];pos+=1
                size=0
                while rbsp[pos]==255:size+=255;pos+=1
                size+=rbsp[pos];pos+=1
                assert size<=len(rbsp)-1-pos
                rows.append((typ,rbsp[pos:pos+size]));pos+=size
                if codec=='vvc' and (typ in (100,101,300) or typ==5 and size<16):private_tids.append(nal[1]&7)
        if codec=='vvc':assert all(t==vcl_tid for t in private_tids),(path,private_tids,vcl_tid)
        key=round(fractions.Fraction(p['pts_time'])*90000)
        assert key not in result,(path,key)
        result[key]=rows;nals_per_packet.append(types)
    return result,nals_per_packet

def verify(source,out,src_codec,dst_codec,selected,allow_drop=False):
    original,_=messages(source,src_codec);actual,nals=messages(out,dst_codec)
    selected={5 if x=='uuid' else x for x in selected};total=0;protected=0
    if not allow_drop:assert set(actual)==set(original),(out,set(original)-set(actual),set(actual)-set(original))
    for pts,rows in original.items():
        expected=[r for r in rows if r[0] in selected]
        if not expected:continue
        protected+=1;total+=len(expected)
        assert pts in actual,('lost protected picture',out,pts)
        got=[r for r in actual[pts] if r[0] in selected]
        # A new encoder may add its own type-5 encoder identification. Every
        # transported message must remain in order; short opaque type-5 and
        # all custom numeric types must also match exactly, without duplicates.
        pos=0
        for row in got:
            if pos<len(expected) and row==expected[pos]:pos+=1
        assert pos==len(expected),(out,pts,expected,got)
        exact=lambda rows:[r for r in rows if r[0]!=5 or len(r[1])<16]
        assert exact(got)==exact(expected),(out,pts,expected,got)
    # Newly generated type 5 is permitted, unrequested custom types are not.
    for rows in actual.values():assert all(t not in {100,101,102,300}-selected for t,_ in rows),(out,rows)
    decoded=[int(line) for line in command([BIN/'naki-sei-decode-probe',out]).splitlines()]
    assert decoded==sorted(actual),(out,decoded,sorted(actual))
    return total,protected,{'packets':len(actual),'protected':protected,'messages':total,'nals':nals}

def execute(server,name,source,src_codec,outputs,extra=None,drop=False,failed=False):
    spec={'input':str(source),'encoder_preset':'ultrafast','frame_log':True,
        'timestamps':{'mode':'source_all'},**(extra or {})}
    if len(outputs)==1:spec.update(outputs[0])
    else:spec['outputs']=outputs
    server.api('/v1/jobs',{'operation_id':'create-'+name,'spec':spec},202)
    final=wait(lambda:(s if (s:=server.api())['state'] in ('completed','completed_partial','failed','stopped') else None),30)
    (RUN/f'{name}-final.json').write_text(json.dumps(final,indent=2)+'\n')
    if failed:
        assert final['state']=='failed' and not final.get('sei_written',0),final
        RESULTS[name]={'final':final};return
    assert final['state']=='completed' and final['supervisor']['exit_code']==0,(name,final['state'],final.get('error'),
        [(b['output_id'],b['state'],b.get('error')) for b in final.get('outputs',[])])
    states=final['outputs'] if len(outputs)>1 else [final];checks=[]
    for o,state in zip(outputs,states):
        count,protected,check=verify(source,pathlib.Path(o['output']),src_codec,
            'vvc' if o.get('encoder')=='libvvenc' else 'h264',o['sei']['types'],drop)
        assert state['sei_written']==count and state['sei_protected_frames']==protected,(name,
            {k:state.get(k) for k in ('sei_written','sei_protected_frames','sei_extracted')},count,protected)
        assert not state.get('inflight_frames',0) and not state.get('queue_items',0),state
        checks.append(check)
    RESULTS[name]={'final':final,'checks':checks}
    print('PASS SEI',name,flush=True);return final

def output(name,codec='libx264',fmt='mpegts',types=('uuid',100,101,300),**extra):
    return {'output':str(RUN/f'{name}.{dict(mpegts="ts",mp4="mp4",flv="flv")[fmt]}'),
        'output_format':fmt,'encoder':codec,'encoder_preset':'faster' if codec=='libvvenc' else 'ultrafast',
        'sei':{'types':list(types)},**extra}

def run():
    global RUN
    RUN=pathlib.Path(tempfile.mkdtemp(prefix='sei-',dir=ROOT/'out'))
    before=source_hashes()
    sources={}
    for codec in ('h264','hevc'):
        raw=RUN/f'base-{codec}.mp4';source=RUN/f'input-{codec}.mp4'
        args=['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v','48',
            '-vf',r'settb=1/90000,setpts=450000+N*3600+gte(N\,20)*180000','-fps_mode','passthrough',
            '-c:v','libx264' if codec=='h264' else 'libx265','-preset','ultrafast','-threads','2']
        args+=['-x264-params','keyint=1000:scenecut=0','-bf','3'] if codec=='h264' else ['-x265-params','pools=1:frame-threads=1:keyint=1000:scenecut=0:bframes=3:log-level=error']
        command(args+[raw]);command([BIN/'naki-sei-fixture',raw,source,'sparse']);sources[codec]=source
    mark=RUN/'mark.png';png(mark)
    server=Server(RUN, 'sei-real')
    try:
        for src_codec,source in sources.items():
            for encoder in ('libx264','libvvenc'):
                for fmt in ('mpegts','mp4','flv'):
                    name=f'{src_codec}-{encoder}-{fmt}'
                    execute(server,name,source,src_codec,[output(name,encoder,fmt,width=128,height=80,
                        watermark={'enabled':True,'resource':{'uri':str(mark)},'placement':{'size':{'width':8,'height':8}}})])
            ts=RUN/f'input-{src_codec}.ts'
            command(['ffmpeg','-v','error','-copyts','-i',source,'-map','0:v:0','-c:v','copy','-mpegts_copyts','1','-muxdelay','0',ts])
            execute(server,f'{src_codec}-annexb',ts,src_codec,[output(f'{src_codec}-annexb')])
        choices=[('a','libx264',('uuid',)),('b','libvvenc',(100,101)),('c','libx264',(300,)),('d','libvvenc',())]
        execute(server,'four-subsets',sources['hevc'],'hevc',[output(f'four-{name}',enc,types=types,id=name) for name,enc,types in choices],{'pace':True})
        overflow=RUN/'overflow.mp4';command([BIN/'naki-sei-fixture',RUN/'base-h264.mp4',overflow,'overflow'])
        execute(server,'overflow',overflow,'h264',[output('overflow')],failed=True)
        all_hevc=RUN/'hevc-all-short-uuid.mp4';command([BIN/'naki-sei-fixture',RUN/'base-hevc.mp4',all_hevc,'all'])
        execute(server,'hevc-first-short-uuid',all_hevc,'hevc',[output('hevc-first-short-uuid','libvvenc')])
        for frames in range(1,6):
            raw=RUN/f'tail-base-{frames}.mp4';source=RUN/f'tail-{frames}.mp4'
            command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',frames,
                '-c:v','libx264','-threads','2','-bf','3',raw])
            command([BIN/'naki-sei-fixture',raw,source,'all'])
            execute(server,f'vvc-tail-{frames}',source,'h264',[output(f'tail-{frames}','libvvenc')])
        name='controller-recovery';o=output(name);source=sources['h264']
        accepted=server.api('/v1/jobs',{'operation_id':'create-recovery','spec':{'input':str(source),**o,
            'pace':True,'timestamps':{'mode':'source_all'}}},202)
        wait(lambda:server.api().get('packets_out',0)>2);server.restart()
        final=wait(lambda:(s if (s:=server.api())['state'] in ('completed','failed') else None),15)
        assert final['job_id']==accepted['job_id'] and final['state']=='completed',final
        count,protected,check=verify(source,pathlib.Path(o['output']),'h264','h264',o['sei']['types'])
        assert final['sei_written']==count and final['sei_protected_frames']==protected
        RESULTS[name]={'final':final,'checks':[check]}
        name='cancel-inflight';o=output(name)
        accepted=server.api('/v1/jobs',{'operation_id':'create-cancel','spec':{'input':str(source),**o,'pace':True}},202)
        wait(lambda:server.api().get('packets_out',0)>2)
        server.api(f'/v1/jobs/{accepted["job_id"]}/stop',{'operation_id':'stop-cancel'},(200,202))
        final=wait(lambda:(s if (s:=server.api())['state'] in ('stopped','failed') else None),8)
        assert final['state']=='stopped' and final['progress']['tracked_video_packets']==final['progress']['tracked_video_frames']==0,final
        RESULTS[name]={'final':final}
    finally:server.close()
    hooks=RUN/'hooks';hooks.mkdir();binaries=RUN/'hook-bin';binaries.mkdir()
    for name in ('naki-live','naki-crash-collector'):shutil.copy2(BIN/name,binaries/name)
    shutil.copy2(BIN/'naki-live-hook-worker',binaries/'naki-live-worker')
    server=Server(RUN, 'sei-overload',binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(hooks),'NAKI_TEST_MEDIA_HOOK_MODE':'sei_pause_video'})
    policy={'intra':{'mode':'follow_source'},'frame_selection':{'mode':'live_drop_only','overload':{
        'signal':'oldest_inflight_ingress_age_ms','levels':[{'enter_ms':10,'sustain_ms':0,'keep_one_in':3}],
        'recover_below_ms':1,'recover_sustain_ms':1000,'max_continuous_degraded_ms':5000,'on_exhausted':'fail_task'}}}
    try:
        final=execute(server,'protected-during-overload',sources['h264'],'h264',[output('protected',types=(100,101,300))],policy,drop=True)
        assert final['frames_dropped_overload']>0 and final['overload_escalations']>0,final
        all_frames=RUN/'all-frames.mp4';command([BIN/'naki-sei-fixture',RUN/'base-h264.mp4',all_frames,'all'])
        final=execute(server,'all-protected',all_frames,'h264',[output('all-protected',types=(100,))],policy)
        assert final['overload_escalations']>0 and not final['frames_dropped_overload'] and final['sei_protected_frames']==48,final
    finally:server.close()
    server=Server(RUN, 'sei-hook-failure',binaries/'naki-live',{'NAKI_TEST_MEDIA_HOOK_DIR':str(hooks),'NAKI_TEST_MEDIA_HOOK_MODE':'video_error'})
    try:execute(server,'hook-failure',sources['h264'],'h264',[output('hook-failure')],failed=True)
    finally:server.close()
    after=source_hashes();assert before==after,'source changed during SEI integration'
    (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','source_hashes':after,'cases':RESULTS},indent=2)+'\n')
    print('PASS actual SEI bytes/type/order/PTS, cross codec, short UUID, suffix, scaling/watermark, subsets, overload and overflow:',RUN,flush=True)

if __name__=='__main__':run()
