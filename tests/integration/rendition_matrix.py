#!/usr/bin/env python3
"""Fixed-count shared-input parity and all-process local resource observations.

CPU is container cgroup usage during POST-to-terminal only. Fixture creation and
independent decoding happen outside that interval. Do not run other tests in the
container concurrently. Virtualized/emulated measurements are not capacity SLAs.
"""
import argparse
import hashlib
import errno
import re
import itertools
import os
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
from pathlib import Path
import tempfile
import time
import uuid
from integration.rendition_process_integration import ROOT, BIN, SDK, ORACLE_FFMPEG, Harness, command, wait, require, frame_rows, decoded_hashes


def cpu():
    return dict((k,int(v)) for k,v in (line.split() for line in Path('/sys/fs/cgroup/cpu.stat').read_text().splitlines()))


def smaps_metrics(text):
    values={}
    for line in text.splitlines():
        key,separator,tail=line.partition(':')
        # The mapping header contains device 00:00; it is not a metric.
        if not separator or not re.fullmatch(r'[A-Za-z_]+',key):continue
        parts=tail.split();require(len(parts)==2 and parts[0].isdigit() and parts[1]=='kB',('invalid smaps metric',line))
        values[key]=int(parts[0])
    require(all(k in values for k in ['Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty']),'smaps data missing; never substitute zero for a failed read')
    return values


def resources(pids):
    result=dict(pss_kib=0,private_kib=0,shared_clean_kib=0,shared_dirty_kib=0,threads=0,fds=0,processes=0,cpu_ticks=0)
    all_processes={}
    for path in Path('/proc').glob('[0-9]*/status'):
        try:
            fields=dict(line.split(':',1) for line in path.read_text().splitlines() if ':' in line)
            all_processes[int(path.parent.name)]=int(fields['PPid'])
        except OSError as error:
            if error.errno not in (errno.ENOENT,errno.ESRCH):raise
    pids=set(pids)
    for _ in range(5):pids.update(pid for pid,parent in all_processes.items() if parent in pids)
    for pid in pids:
        try:
            fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
            if fields[0]=='Z':memory=dict(Pss=0,Private_Clean=0,Private_Dirty=0,Shared_Clean=0,Shared_Dirty=0);fds=0
            else:
                memory=smaps_metrics(Path(f'/proc/{pid}/smaps_rollup').read_text());fds=len(list(Path(f'/proc/{pid}/fd').iterdir()))
            ticks=sum(int(fields[i]) for i in [11,12,13,14]);threads=int(fields[17])
        except OSError as error:
            # Only a process that actually vanished may be omitted. Permission
            # or format failures invalidate the measurement, not produce zero.
            if error.errno in (errno.ENOENT,errno.ESRCH):continue
            raise
        result['pss_kib']+=memory['Pss'];result['private_kib']+=memory['Private_Clean']+memory['Private_Dirty']
        result['shared_clean_kib']+=memory['Shared_Clean'];result['shared_dirty_kib']+=memory['Shared_Dirty']
        result['cpu_ticks']+=ticks;result['threads']+=threads;result['fds']+=fds;result['processes']+=1
    require(result['processes']>0 and result['pss_kib']>0 and result['threads']>0 and result['fds']>0,'resource measurement unavailable')
    return result


def check_resources():
    fixture='00400000-7fffffff ---p 00000000 00:00 0 [rollup]\nPss: 23 kB\nPrivate_Clean: 4 kB\nPrivate_Dirty: 7 kB\nShared_Clean: 9 kB\nShared_Dirty: 3 kB\n'
    require(smaps_metrics(fixture)['Pss']==23,'smaps header regression')
    try:smaps_metrics('permission denied')
    except AssertionError:pass
    else:raise AssertionError('missing metrics accepted')
    sample=resources([os.getpid()]);require(sample['cpu_ticks']>0,'CPU reader returned a false zero')


def vvc_hashes(directory,path,count,width,height):
    decoded=json.loads(command([SDK/'ffprobe','-v','error','-threads','2','-select_streams','v:0','-show_frames','-show_entries','frame=pts_time','-of','json',path]))['frames']
    require(len(decoded)==count,'independent native VVC decode count')
    indices=directory/(path.stem+'-indices.txt');indices.write_text('\n'.join(map(str,range(count)))+'\n')
    raw=directory/(path.stem+'-decoded.yuv');manifest=directory/(path.stem+'-decoded.json')
    command([BIN/'naki-quality-video-export',path,raw,indices,manifest,width,height,'decoded'],90)
    data=raw.read_bytes();stride=3*width*height;require(len(data)==count*stride,'native VVC decoded plane bytes')
    return [hashlib.sha256(data[i*stride:(i+1)*stride]).hexdigest() for i in range(count)],[round(float(f['pts_time'])*1000000) for f in decoded]


def quantiles(values):
    values=sorted(values);require(values,'zero latency samples')
    return {name:values[min(len(values)-1,int((len(values)-1)*q+.5))] for name,q in [('p50_ms',.5),('p95_ms',.95),('p99_ms',.99)]}|dict(count=len(values),max_ms=values[-1])


def case(directory,source,frames,backend,async_decode,early,pump=False,advanced=False,large=False):
    h=Harness(directory)
    try:
        h.start()
        outputs=[dict(id=f'out{i}',output=str(directory/f'out{i}.mp4'),output_format='mp4',width=(640 if i==0 else 320) if large else (160 if i==0 else 80),height=(360 if i==0 else 180) if large else (96 if i==0 else 48),encoder_preset='ultrafast',audio_copy=False,encoder_threads=1) for i in range(2)]
        config=dict(input=str(source),outputs=outputs,rendition_backend=backend,pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,decoder_async=async_decode,decoder_early_output=early,decoder_pump=pump,frame_log=True,timestamps={'mode':'source_all'})
        if advanced:
            config.update(rate_control='constrained_crf',crf=32,maxrate=100000000,output_fps=5,
                          intra=dict(mode='follow_source'),encoder_max_future_frames=4,codec_trace=True,decoder_pump=True,
                          encoder_bit_depth=8,encoder_input_bit_depth=8,encoder_native8_reference=True)
            for o in outputs:o.update(encoder_threads=8,encoder='libvvenc',encoder_preset='faster',encoder_profile='live_3b_2s_speed')
        (directory/'config.json').write_text(json.dumps(config,indent=2)+'\n')
        service_before=resources([h.server.pid]);before=cpu();began=time.monotonic();h.api(body=config,expected=202);samples=[];rejected_capabilities=[]
        def terminal():
            s=h.api();pids=[h.server.pid,s.get('supervisor',{}).get('worker_pid',0)]+[x.get('rendition_pid',0) for x in s.get('outputs',[])]
            outputs=s.get('outputs',[])
            if advanced and not rejected_capabilities and outputs and outputs[0].get('rendition_pid') and outputs[0].get('packets_out'):
                body=dict(operation_id=uuid.uuid4().hex,action='request_random_access',expected_revision=s['topology_revision'],expected_generation=outputs[0]['rendition_generation'],target=dict(component='rendition',output_id=outputs[0]['output_id']))
                rejected_capabilities.append(h.api(f'/v1/jobs/{s["job_id"]}/updates',body,422))
            for output in outputs:require('queue_video_items' in output and 'queue_bytes' in output,'queue telemetry missing')
            ages=[x.get('progress',{}).get('oldest_video_inflight_age_ms') for x in outputs]
            known_ages=[x for x in ages if x is not None]
            samples.append(dict(oldest_video_inflight_age_ms=max(known_ages) if known_ages else None,wall_s=time.monotonic()-began,**resources(pid for pid in pids if pid),frames=s.get('frames_decoded',0),
                                queued=sum(x['queue_video_items'] for x in outputs),queue_bytes=sum(x['queue_bytes'] for x in outputs),
                                retained=sum(x.get('raw_video_retained',0) for x in outputs),delivery_frames=sum(x.get('delivery_inflight',{}).get('frames',0) for x in outputs)))
            (directory/'latest-state.json').write_text(json.dumps(s,indent=2)+'\n')
            return s if s['state'] in ('completed','completed_partial','failed','stopped') else None
        final=wait(terminal,90);elapsed=time.monotonic()-began;after=cpu();service_after=resources([h.server.pid])
        require(service_after['cpu_ticks']>service_before['cpu_ticks'],'whole-service CPU counter did not advance')
        require(final['state']=='completed' and final['frames_decoded']==frames,(final['state'],final.get('error'),final['frames_decoded']))
        require(final['shared_pool_occupied_slots']==0,'raw pool did not close')
        sustained=None
        if large:
            middle=[x for x in samples if 5<=x['wall_s']<=25];require(len(middle)>=2,'sustained observation window missing')
            steady=(middle[-1]['frames']-middle[0]['frames'])/(middle[-1]['wall_s']-middle[0]['wall_s'])
            early=[x['queued'] for x in samples if 5<=x['wall_s']<=10];late=[x['queued'] for x in samples if 24<=x['wall_s']<=29]
            require(early and late,'sustained queue windows missing');growth=sum(late)/len(late)-sum(early)/len(early)
            require(steady>=24 and growth<=4,('25fps sustained target or growing backlog',steady,growth))
            sustained=dict(target_fps=25,minimum_observed_fps=24,steady_fps=steady,queue_mean_growth_frames=growth)
        results=[]
        for output in final['outputs']:
            rows=frame_rows(output['log_path']);expected=list(range(1,frames+1)) if not advanced else None
            if expected:require([r['frame_id'] for r in rows]==expected,'source identity loss/duplication/order')
            else:require(sorted(r['frame_id'] for r in rows)==list(range(1,frames+1,5)),'fixed advanced selection identity set')
            spec=next(o for o in outputs if o['id']==output['output_id'])
            if advanced:
                hashes,times=vvc_hashes(directory,Path(spec['output']),frames//5,spec['width'],spec['height']);require(times==sorted(r['pts_us'] for r in rows),'VVC decoded display order/PTS mapping')
            else:hashes=decoded_hashes(spec['output'])
            require(len(hashes)==(frames//5 if advanced else frames),'independent output frame count')
            if advanced:
                require(all('rc_qp' in r and 'encoder_future_frames' in r for r in rows),'advanced capability silently lost')
            results.append(dict(output_id=output['output_id'],hashes=hashes,latency=quantiles([r['ingress_to_mux_ms'] for r in rows])))
        return dict(config=config,final=final,sustained=sustained,rejected_capabilities=rejected_capabilities,elapsed_s=elapsed,decoded_fps=frames/elapsed,service_cpu_us=(service_after['cpu_ticks']-service_before['cpu_ticks'])*1000000//os.sysconf('SC_CLK_TCK'),cgroup_cpu_us=after['usage_usec']-before['usage_usec'],samples=samples,outputs=results)
    finally:h.close()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--quick',action='store_true');parser.add_argument('--advanced',action='store_true');parser.add_argument('--sustain',action='store_true');args=parser.parse_args();check_resources()
    run=Path(tempfile.mkdtemp(prefix='rendition-matrix-',dir=ROOT/'out'));frames=750 if args.sustain else 75;report=dict(cases=[],boundary='service packet ingress to actual mux flush; x86 Linux QEMU functional environment; no capacity claim')
    identity={name:hashlib.sha256((BIN/name).read_bytes()).hexdigest() for name in ['naki-live','naki-live-worker']};report['binaries']=identity
    golden={}
    try:
        fixtures=[('hevc',3)] if args.advanced else [('h264',7 if args.sustain else 3)] if args.quick or args.sustain else list(itertools.product(['h264','hevc'],[3,7]))
        for codec,bframes in fixtures:
            source=run/f'{codec}-b{bframes}.ts';encoder='libx264' if codec=='h264' else 'libx265'
            options=['-x264-params',f'bframes={bframes}:b-adapt=0:nal-hrd=cbr:force-cfr=1','-b:v','300k','-minrate','300k','-maxrate','300k','-bufsize','300k'] if codec=='h264' else ['-x265-params',f'pools=1:frame-threads=1:bframes={bframes}:b-adapt=0:rc-lookahead=16:log-level=error']
            command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i',('testsrc2=size=640x360:rate=25' if args.sustain else 'testsrc2=size=160x96:rate=25'),'-frames:v',frames,'-pix_fmt',('yuv420p10le' if codec=='hevc' and bframes==7 else 'yuv420p'),'-c:v',encoder,'-preset','ultrafast','-threads','2','-g','25',*options,'-f','mpegts',source])
            settings=[('thread',True,True),('process',True,True)] if args.advanced or args.sustain else itertools.product(['thread','process'],[False,True],[False,True])
            for backend,async_decode,early in settings:
                directory=run/f'{codec}-b{bframes}-{backend}-a{int(async_decode)}-e{int(early)}';directory.mkdir()
                result=case(directory,source,frames,backend,async_decode,early,advanced=args.advanced,large=args.sustain)
                for output in result['outputs']:
                    key=(codec,bframes,output['output_id']);reference=golden.setdefault(key,output['hashes']);require(reference==output['hashes'],('decoded pixel parity',key,backend,async_decode,early))
                report['cases'].append(result);(directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        require(identity=={name:hashlib.sha256((BIN/name).read_bytes()).hexdigest() for name in identity},'binary changed during matrix')
        report['passed']=True
    finally:(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(run),passed=True,cases=len(report['cases']))))


if __name__=='__main__':main()
