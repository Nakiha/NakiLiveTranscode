#!/usr/bin/env python3
"""Real one-decode fan-out, dynamic fixed outputs, isolation and takeover."""
import binascii
import copy
import http.server
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
RUN=pathlib.Path(tempfile.mkdtemp(prefix='multi-output-',dir=ROOT/'out'))
PROBE=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT','/root/.cache/naki-live-transcode-n9'))/'install/bin/ffprobe'
def command(args,timeout=40):
    p=subprocess.run(list(map(str,args)),capture_output=True,timeout=timeout)
    assert p.returncode==0,(args,p.returncode,p.stderr[-3000:]);return p.stdout
def wait(fn,timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        v=fn()
        if v:return v
        time.sleep(.03)
    raise AssertionError('condition timed out')
def png(path,color):
    def chunk(k,v):return struct.pack('>I',len(v))+k+v+struct.pack('>I',binascii.crc32(k+v)&0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,6,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes([*color,255])*8)*8))+chunk(b'IEND',b''))
def video(path):
    data=command([BIN/'naki-output-video-probe',path]);(RUN/(path.name+'.decoded.json')).write_bytes(data);return json.loads(data)
def watermark(uri,required=True):
    return {'enabled':True,'resource':{'uri':str(uri)},'startup':{'required':required},
            'placement':{'anchor':'top_left','size':{'width':16,'height':16,'fit':'stretch'}}}
def output(name,fmt='mp4',w=160,h=120,**kw):
    cfg=dict(id=name,output=str(RUN/f'{name}.{fmt}'),output_format=fmt,width=w,height=h,encoder_preset='ultrafast');cfg.update(kw);return cfg
def released(state):
    assert all(not b['queue_items'] and not b['queue_bytes'] and not b['inflight_frames'] and not b['inflight_bytes'] for b in state['outputs']),state
class Stall:
    def __enter__(self):
        self.stop=threading.Event();self.connected=threading.Event();self.socket=socket.socket();self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        self.socket.bind(('127.0.0.1',0));self.socket.listen(1);self.socket.settimeout(10)
        self.url=f'tcp://127.0.0.1:{self.socket.getsockname()[1]}?send_buffer_size=1024'
        def accept():
            try:
                conn,_=self.socket.accept()
                with conn:self.connected.set();self.stop.wait(12)
            except OSError:pass
        self.thread=threading.Thread(target=accept,daemon=True);self.thread.start();return self
    def __exit__(self,*_):self.stop.set();self.socket.close();self.thread.join(timeout=3);assert not self.thread.is_alive()
def audio_payloads(path):
    data=command(['ffmpeg','-v','error','-i',path,'-map','0:a:0','-c:a','copy','-f','adts','-']);items=[];pos=0
    while pos<len(data):
        assert data[pos]==255 and data[pos+1]&0xf6==0xf0
        size=((data[pos+3]&3)<<11)|(data[pos+4]<<3)|(data[pos+5]>>5);header=7 if data[pos+1]&1 else 9
        assert size>=header and pos+size<=len(data);items.append(data[pos+header:pos+size]);pos+=size
    return items
def main():
    results={};red=RUN/'red.png';blue=RUN/'blue.png';png(red,(255,0,0));png(blue,(0,0,255))
    sample=RUN/'sample.ts'
    command(['ffmpeg','-v','error','-f','lavfi','-i','color=black:size=320x180:rate=25:duration=5','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=5',
             '-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','0','-c:a','aac','-b:a','64k',
             '-muxdelay','0','-muxpreload','0','-f','mpegts',sample])
    # A success fixture must not rely on owner scheduling to absorb a full
    # 16-packet audio PES burst plus queued video in the shared 16-item budget.
    # Keep all media and assert the packet-order precondition; pressure cases
    # below and the mixed-queue unit test retain the hard admission contract.
    packets=json.loads(command(['ffprobe','-v','error','-show_packets','-show_entries','packet=codec_type','-of','json',sample]))['packets']
    burst=longest=0
    for packet in packets:
        burst=burst+1 if packet['codec_type']=='audio' else 0;longest=max(longest,burst)
    assert longest<=4,longest
    results['success-fixture-audio-burst']={'longest_consecutive_audio_packets':longest,'limit':4}
    s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close();base=f'http://127.0.0.1:{port}';server=None;logs=[]
    def api(body=None,path='/v1/job',code=200):
        req=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        try:r=urllib.request.urlopen(req,timeout=4)
        except urllib.error.HTTPError as e:r=e
        with r:
            value=json.load(r);assert r.status==code,(path,r.status,value);return value
    def serve():
        nonlocal server
        log=(RUN/f'server-{len(logs)}.jsonl').open('wb');err=(RUN/f'server-{len(logs)}.stderr').open('wb');logs.extend([log,err])
        server=subprocess.Popen([str(BIN/'naki-live'),'--port',str(port),'--run-dir',str(RUN/'logs')],stdout=log,stderr=err,cwd=ROOT)
        def online():
            assert server.poll() is None
            try:return api(path='/health')
            except OSError:return False
        wait(online)
    def start(outputs,source=sample,**kw):
        cfg=dict(input=str(source),outputs=outputs,pace=True,timestamps={'mode':'source_all'},frame_log=True);cfg.update(kw);return api(cfg,code=202)
    def done(expected='completed',timeout=20):
        state=wait(lambda:(v if (v:=api())['state'] in ('completed','completed_partial','failed','stopped') else None),timeout)
        (RUN/f"terminal-{state['job_id']}.json").write_text(json.dumps(state,indent=2)+'\n')
        assert state['state']==expected,state;released(state);return state
    def traces(state):
        prefix=pathlib.Path(state['log_path']);paths=wait(lambda:list(prefix.parent.glob(prefix.name+'.*.jsonl')))
        return [json.loads(line) for p in paths for line in p.read_text().splitlines() if line]
    try:
        serve()
        # Strict static validation before media allocation.
        a=output('valid-a');b=output('valid-b')
        bads=[{'input':str(sample),'outputs':[a]}, {'input':str(sample),'outputs':[a]*5},
              {'input':str(sample),'outputs':[a,dict(b,id='valid-a')]}, {'input':str(sample),'outputs':[a,dict(b,output=a['output'])]},
              {'input':str(sample),'outputs':[a,dict(b,queue_items=17)]}, {'input':str(sample),'outputs':[a,dict(b,queue_bytes=1)]},
              {'input':str(sample),'outputs':[a,dict(b,extra=True)]}, {'input':str(sample),'outputs':[a,b],'output':str(RUN/'ambiguous.ts')}]
        for bad in bads:api(bad,code=400)
        results['static-contract-rejections']=len(bads)
        api({'input':str(sample),'output':str(RUN/'legacy-audio.ts'),'pace':True,'encoder_preset':'ultrafast','timestamps':{'mode':'source_all'}},code=202)
        legacy=wait(lambda:(s if (s:=api())['state'] in ('completed','failed') else None))
        assert legacy['state']=='completed';original_audio=audio_payloads(sample);legacy_audio=audio_payloads(RUN/'legacy-audio.ts')
        assert len(legacy_audio)+legacy['audio_dropped']==len(original_audio) and not legacy['audio_unsupported'],legacy
        results['legacy-audio-reference']=legacy
        branches=[output('two-a'),output('two-b','mpegts',192,144)]
        start(branches);state=done();assert state['frames_decoded']==125 and all(o['packets_out']==125 for o in state['outputs']),state
        assert state['decode']['count']==125;results['two-shared-decode']=state
        branches=[output('four-red',watermark=watermark(red)),output('four-blue','flv',192,144,watermark=watermark(blue)),
                  output('four-bare','mpegts',128,96),output('four-vvc','mpegts',256,144,encoder='libvvenc',watermark=watermark(red))]
        branches[3]['encoder_preset']='faster'
        start(branches);state=done();assert state['frames_decoded']==125 and state['decode']['count']==125
        assert all(o['packets_out']==125 and o['frames_decoded']==125 for o in state['outputs']),state
        assert max(o['first_video_mux_us'] for o in state['outputs'])<min(o['last_video_mux_us'] for o in state['outputs']),state
        rows=traces(state);frames=[r for r in rows if r['event']=='frame']
        assert len(frames)==500
        for cfg in branches:
            branch_rows=[r for r in frames if r['output_id']==cfg['id']];assert sorted(r['frame_id'] for r in branch_rows)==list(range(1,126))
            pixels=video(pathlib.Path(cfg['output']));assert len(pixels)==125
            for frame in pixels:
                assert (frame['width'],frame['height'])==(cfg['width'],cfg['height']) and max(frame['center'])<35
                r,g,b=frame['mark']
                if cfg['id'] in ('four-red','four-vvc'):assert r>150 and b<80,(cfg['id'],frame)
                elif cfg['id']=='four-blue':assert b>150 and r<80,frame
                else:assert max(frame['mark'])<35,frame
            if cfg['id']=='four-vvc':assert all(f['format']=='yuv420p10le' for f in pixels) and any(f['picture']==3 for f in pixels)
        for cfg in branches[1:]:
            got=audio_payloads(pathlib.Path(cfg['output']));offset=0
            for payload in got:
                while offset<len(original_audio) and original_audio[offset]!=payload:offset+=1
                assert offset<len(original_audio),(cfg['id'],len(got));offset+=1
            status=next(o for o in state['outputs'] if o['output_id']==cfg['id'])
            assert len(got)==status['audio_packets_out'] and len(got)+status['audio_dropped']==len(original_audio),status
            assert not status.get('audio_unsupported',0)
        results['four-private-owners-cow-audio']=state
        # Full independent revisions with an unchanged sibling and no bare required frames.
        branches=[output('hot-a',watermark=watermark(red)),output('hot-b',watermark=watermark(blue))]
        start(branches);wait(lambda:len(api().get('outputs',[]))==2 and all(o['packets_out']>=5 for o in api()['outputs']))
        api({'revision':2,'watermark':watermark(blue)},path='/v1/job/watermark',code=409)
        api({'output_id':'missing','revision':2,'watermark':watermark(blue)},path='/v1/job/watermark',code=409)
        api({'output_id':'hot-a','revision':2,'watermark':watermark(blue)},path='/v1/job/watermark',code=202)
        wait(lambda:api()['outputs'][0]['watermark']['applied_revision']==2)
        assert api()['outputs'][1]['watermark']['accepted_revision']==1
        api({'output_id':'hot-b','revision':2,'watermark':watermark(red)},path='/v1/job/watermark',code=202)
        state=done();assert all(o['watermark']['applied_revision']==2 for o in state['outputs'])
        for cfg in branches:
            pixels=video(pathlib.Path(cfg['output']));assert len(pixels)==125
            assert all(max(f['mark'][0],f['mark'][2])>150 and max(f['center'])<35 for f in pixels)
            assert pixels[0]['mark'][0]>150 if cfg['id']=='hot-a' else pixels[0]['mark'][2]>150
            assert pixels[-1]['mark'][2]>150 if cfg['id']=='hot-a' else pixels[-1]['mark'][0]>150
        results['output-id-watermark-revisions']=state
        # Pressure and permanent open failures release only the affected branch.
        start([output('pressure-ok'),output('pressure-bad',queue_bytes=4096)])
        wait(lambda:api().get('outputs') and api()['outputs'][1]['state']=='failed')
        api({'output_id':'pressure-bad','revision':2,'watermark':watermark(blue,False)},path='/v1/job/watermark',code=409)
        state=done('completed_partial')
        assert state['outputs'][0]['packets_out']==125 and state['outputs'][1]['state']=='failed';results['queue-pressure-isolation']=state
        start([output('missing-parent',output=str(RUN/'missing'/'a.mp4')),output('all-pressure',queue_bytes=4096)])
        results['all-output-failure']=done('failed')
        # The receiver accepts a real TCP connection and never reads any bytes.
        with Stall() as stalled:
            slow=output('slow','mpegts',640,360,output=stalled.url,io_timeout_ms=300,max_lag_ms=1000,bitrate=10000000)
            # High entropy / intra source ensures the small socket window fills.
            noise=RUN/'noise.mp4';command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=640x360:rate=25:duration=5','-c:v','libx264','-preset','ultrafast','-threads','2','-g','1',noise])
            accepted=start([output('slow-healthy'),slow],source=noise)
            assert stalled.connected.wait(5)
            observed=wait(lambda:(s if (s:=api()).get('outputs') and s['outputs'][1]['state']=='failed' and s['outputs'][0]['packets_out']>10 else None))
            assert observed['supervisor']['worker_pid']==accepted['supervisor']['worker_pid']
            state=done('completed_partial');assert state['outputs'][0]['packets_out']==125
            assert 'deadline' in state['outputs'][1]['error'] or 'timed out' in state['outputs'][1]['error'].lower(),state
            results['blocked-tcp-isolation']=state
        # One HEVC elementary stream changes SPS dimensions and 8->10->8 bit.
        raw=RUN/'dynamic.hevc';parts=[]
        for index,(w,h,ten,color) in enumerate([(64,48,False,'black'),(96,64,False,'gray'),(128,96,True,'white'),(64,48,False,'black')]):
            part=RUN/f'part-{index}.hevc'
            command(['ffmpeg','-v','error','-f','lavfi','-i',f'color={color}:size={w}x{h}:rate=25','-frames:v','20','-pix_fmt','yuv420p10le' if ten else 'yuv420p',
                     '-color_primaries','bt709','-color_trc','bt709','-colorspace','bt709','-c:v','libx265','-preset','ultrafast','-threads','2',
                     '-x265-params','pools=2:frame-threads=1:keyint=1:bframes=0:repeat-headers=1:log-level=error','-f','hevc',part]);parts.append(part.read_bytes())
        raw.write_bytes(b''.join(parts));dyn_video=RUN/'dynamic-video.ts';dynamic=RUN/'dynamic.ts'
        command(['ffmpeg','-v','error','-fflags','+genpts','-r','25','-i',raw,'-c:v','copy','-f','mpegts',dyn_video])
        command(['ffmpeg','-v','error','-i',dyn_video,'-f','lavfi','-i','sine=frequency=600:sample_rate=48000:duration=3.2','-map','0:v','-map','1:a','-c:v','copy','-c:a','aac','-shortest','-f','mpegts',dynamic])
        source_frames=video(dynamic);assert len(source_frames)==80 and len({(f['width'],f['height'],f['format']) for f in source_frames})==3
        branches=[output('dynamic-mp4',watermark=watermark(red)),output('dynamic-flv','flv',192,144,watermark=watermark(blue)),
                  output('dynamic-ts','mpegts',128,96),output('dynamic-vvc','mpegts',256,144,encoder='libvvenc',watermark=watermark(red))]
        branches[3]['encoder_preset']='faster';start(branches,source=dynamic);state=done();assert state['frames_decoded']==80
        assert all(o['filter_generations']==4 and o['packets_out']==80 for o in state['outputs']),state
        for cfg in branches:
            frames=video(pathlib.Path(cfg['output']));assert len(frames)==80
            for f,src in zip(frames,source_frames):
                assert (f['width'],f['height'])==(cfg['width'],cfg['height'])
                assert abs(f['pts']*f['tb_num']/f['tb_den']-src['pts']*src['tb_num']/src['tb_den'])<.0011,(cfg['id'],f,src)
                assert max(abs(a-b) for a,b in zip(f['center'],src['center']))<25,(cfg['id'],f,src)
                if 'watermark' in cfg:assert max(f['mark'][0],f['mark'][2])>150
            if cfg['id']=='dynamic-vvc':assert all(f['format']=='yuv420p10le' for f in frames) and any(f['picture']==3 for f in frames)
            if cfg['output_format']!='mp4':assert audio_payloads(pathlib.Path(cfg['output']))
        results['dynamic-sps-fixed-output']=state
        gap=RUN/'gap.ts'
        command(['ffmpeg','-v','error','-f','lavfi','-i','color=black:size=96x64:rate=25','-frames:v','8',
                 '-vf',r'settb=1/90000,setpts=PTS+if(gte(N\,4)\,450000\,0)','-enc_time_base','1:90000','-fps_mode','passthrough',
                 '-c:v','libx264','-preset','ultrafast','-threads','2','-g','1','-bf','0','-f','mpegts',gap])
        start([output('gap-a',queue_media_ms=400),output('gap-b','mpegts',queue_media_ms=400)],source=gap,pace=False)
        state=done();assert state['frames_decoded']==8 and all(o['packets_out']==8 for o in state['outputs'])
        original=video(gap)
        for name,fmt in [('gap-a','mp4'),('gap-b','mpegts')]:
            frames=video(RUN/f'{name}.{fmt}')
            assert len(frames)==8
            assert all(abs(f['pts']*f['tb_num']/f['tb_den']-s['pts']*s['tb_num']/s['tb_den'])<.00002 for f,s in zip(frames,original))
        results['source-gap-not-queue-backlog']=state
        # A required asset gates the common reader, not just its own branch.
        class HeldAsset(http.server.BaseHTTPRequestHandler):
            began=threading.Event();release=threading.Event()
            def log_message(self,*_):pass
            def do_GET(self):
                self.began.set();self.release.wait(5);data=red.read_bytes()
                self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers()
                try:self.wfile.write(data)
                except OSError:pass
        assets=http.server.ThreadingHTTPServer(('127.0.0.1',0),HeldAsset);threading.Thread(target=assets.serve_forever,daemon=True).start()
        try:
            start([output('gate-a',watermark=watermark(f'http://127.0.0.1:{assets.server_port}/red')),output('gate-b',watermark=watermark(blue))])
            assert HeldAsset.began.wait(3);waiting=wait(lambda:(s if (s:=api())['state']=='waiting_assets' else None))
            assert waiting['startup']['input_open_started_ms'] is None and not waiting['frames_decoded']
            HeldAsset.release.set();state=done();assert all(o['watermark']['applied_revision']==1 and o['packets_out']==125 for o in state['outputs'])
            results['common-required-startup-gate']=state
        finally:HeldAsset.release.set();assets.shutdown();assets.server_close()
        start([output('gate-failed',watermark=watermark(RUN/'does-not-exist.png')),output('gate-survivor')])
        state=done('completed_partial');assert state['outputs'][0]['state']=='failed' and state['outputs'][0]['packets_out']==0 and state['outputs'][1]['packets_out']==125
        results['required-startup-branch-failure']=state
        start([output('all-gate-a',watermark=watermark(RUN/'missing-a.png')),output('all-gate-b',watermark=watermark(RUN/'missing-b.png'))],source=RUN/'never-open-this-input.ts')
        state=done('failed');assert state['startup']['input_open_started_ms'] is None and not state['packets_in']
        results['all-required-gates-fail-before-input']=state
        # Mid-stream HDR change cannot produce bare required frames or pretend tone mapping.
        hdr_part=RUN/'hdr-part.hevc';hdr_raw=RUN/'hdr-change.hevc';hdr_stream=RUN/'hdr-change.ts'
        command(['ffmpeg','-v','error','-f','lavfi','-i','color=black:size=64x48:rate=25','-frames:v','20','-pix_fmt','yuv420p10le',
                 '-color_primaries','bt2020','-color_trc','smpte2084','-colorspace','bt2020nc','-c:v','libx265','-preset','ultrafast','-threads','2',
                 '-x265-params','pools=2:frame-threads=1:keyint=1:bframes=0:repeat-headers=1:log-level=error','-f','hevc',hdr_part])
        hdr_raw.write_bytes(parts[0]+hdr_part.read_bytes());command(['ffmpeg','-v','error','-fflags','+genpts','-r','25','-i',hdr_raw,'-c:v','copy','-f','mpegts',hdr_stream])
        start([output('hdr-required-a',watermark=watermark(red)),output('hdr-required-b',watermark=watermark(blue))],source=hdr_stream)
        state=done('failed');assert all(o['state']=='failed' and 0<o['packets_out']<=20 for o in state['outputs'])
        for cfg in ('hdr-required-a','hdr-required-b'):
            frames=video(RUN/f'{cfg}.mp4');assert all(max(f['mark'][0],f['mark'][2])>150 for f in frames)
        results['dynamic-required-hdr-rejection']=state
        # Same worker and per-output state survive controller SIGKILL, then cancel.
        branches=[output('recover-a','mpegts'),output('recover-b','mpegts',192,144)]
        accepted=start(branches);wait(lambda:api().get('outputs') and all(o['packets_out']>=10 for o in api()['outputs']))
        old=api();sizes=[pathlib.Path(b['output']).stat().st_size for b in branches];pid=accepted['supervisor']['worker_pid']
        server.kill();server.wait(timeout=4);time.sleep(.5)
        assert all(pathlib.Path(b['output']).stat().st_size>size for b,size in zip(branches,sizes))
        serve();state=wait(lambda:(s if (s:=api()).get('job_id')==accepted['job_id'] and s['supervisor']['recovered'] else None))
        assert state['supervisor']['worker_pid']==pid and state['frames_decoded']>old['frames_decoded'],state
        api({'output_id':'recover-a','revision':2,'watermark':watermark(red,False)},path='/v1/job/watermark',code=202)
        wait(lambda:api()['outputs'][0]['watermark']['applied_revision']==2)
        api({},path='/v1/job/stop',code=202);state=done('stopped');assert all(o['state']=='stopped' for o in state['outputs'])
        results['same-worker-takeover-cancel']=state
        (RUN/'COMPLETE.json').write_text(json.dumps({'status':'passed','cases':results},indent=2)+'\n')
        print(f'Multi-output checks passed: {RUN}')
    finally:
        if server and server.poll() is None:server.send_signal(signal.SIGTERM);server.wait(timeout=8)
        for stream in logs:stream.close()
if __name__=='__main__':main()
