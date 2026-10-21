"""Construct and verify actual input reorder structures, then compare decoders.

No performance claims: these small generated clips exercise correctness at EOF,
GOP boundaries and across flush. Every optimized output is compared with the
unchanged decoder; pixels also have an independent system-FFmpeg SHA256 oracle.
"""
import hashlib
import itertools
import json
import pathlib
import struct
import subprocess


def run_matrix(probe, directory, environment):
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    base = dict(environment)
    for key in ('FFMPEG_LIVE_IDR_EARLY', 'FFMPEG_LIVE_POC_EARLY', 'FFMPEG_LIVE_AUTO_MIN'):
        base.pop(key, None)
    enabled = dict(base, FFMPEG_LIVE_IDR_EARLY='1', FFMPEG_LIVE_POC_EARLY='1', FFMPEG_LIVE_AUTO_MIN='2')
    results = []

    def command(args, env=base, timeout=90):
        args = list(map(str, args))
        with (directory/'commands.jsonl').open('a') as log:
            log.write(json.dumps(dict(argv=args,early=env is enabled))+'\n')
        with (directory/'commands.log').open('a') as log:
            return subprocess.run(args, env=env, stdout=log, stderr=log, check=True, timeout=timeout)

    cases = [('hevc', b, opened, pyramid, 8) for b, opened, pyramid in itertools.product((3,7),(0,1),(0,1))]
    cases += [('hevc', 7, opened, 1, 10) for opened in (0,1)]
    cases += [('h264', b, opened, 1, 8) for b, opened in itertools.product((3,7),(0,1))]
    for codec, bframes, opened, pyramid, depth in cases:
        name = f'{codec}-b{bframes}-open{opened}-pyramid{pyramid}-{depth}bit'
        media = directory/(name+'.mp4')
        params = f'keyint=48:min-keyint=48:scenecut=0:open-gop={opened}:bframes={bframes}:b-adapt=0:'
        params += f'b-pyramid={pyramid if codec=="hevc" else 2}'
        if codec == 'hevc':
            params += ':pools=2:frame-threads=2:log-level=error'
        command(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x180:rate=60',
                 '-frames:v','192','-vf','setpts=PTS+2/TB','-fps_mode','passthrough',
                 '-pix_fmt','yuv420p10le' if depth==10 else 'yuv420p','-c:v','libx265' if codec=='hevc' else 'libx264',
                 '-preset','fast','-threads','2','-x265-params' if codec=='hevc' else '-x264-params',params,media])
        # Do not propagate the fork's LD_LIBRARY_PATH into distribution binaries.
        system = dict(base);system.pop('LD_LIBRARY_PATH',None)
        info = json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
            '-show_frames','-show_packets','-show_entries','frame=pict_type,pts_time,pkt_pos:packet=pos,size',
            '-of','json',str(media)],env=system,text=True,timeout=30))
        # ffprobe interleaves sections when both are requested.
        frames = [f for f in info['packets_and_frames'] if f['type']=='frame']
        packets = [p for p in info['packets_and_frames'] if p['type']=='packet']
        assert len(frames)==len(packets)==192, name
        types = ''.join(f['pict_type'] for f in frames)
        runs = [len(s) for s in types.replace('I','P').split('P') if s]
        assert max(runs)==bframes and all(set(s)<=set('B') for s in types.replace('I','P').split('P')), (name,types)
        assert sum(f['pict_type']=='I' for f in frames)==4, (name,types)
        data=media.read_bytes();counts={};reference_b=0
        by_position={int(f['pkt_pos']):f['pict_type'] for f in frames}
        for packet in packets:
            pos=int(packet['pos']);end=pos+int(packet['size']);reference=False
            while pos<end:
                size=struct.unpack_from('>I',data,pos)[0];pos+=4
                assert size>0 and pos+size<=end
                kind=(data[pos]>>1)&63 if codec=='hevc' else data[pos]&31
                counts[kind]=counts.get(kind,0)+1
                if codec=='hevc' and kind<32:reference |= kind%2==1
                if codec=='h264' and kind in (1,5):reference |= bool(data[pos]&0x60)
                pos+=size
            assert pos==end
            reference_b += reference and by_position[int(packet['pos'])]=='B'
        if codec=='hevc':
            assert counts.get(20,0)>=1, (name,counts)
            if opened:
                assert counts.get(21,0)==3 and counts.get(8,0)+counts.get(9,0)>0, (name,counts)
            else:
                assert counts.get(20,0)==4 and not any(counts.get(k,0) for k in (21,8,9)), (name,counts)
            assert (reference_b>0)==bool(pyramid), (name,reference_b,counts)
        else:
            assert counts.get(5,0)==(1 if opened else 4), (name,counts)
        oracle=directory/(name+'-system.framemd5')
        command(['ffmpeg','-v','error','-i',media,'-map','0:v:0','-fps_mode','passthrough',
                 '-c:v','rawvideo','-threads','2','-f','framemd5','-hash','sha256',oracle],env=system)
        hashes=[line.split(',')[-1].strip() for line in oracle.read_text().splitlines() if line and not line.startswith('#')]
        assert len(hashes)==192
        comparisons=[]
        for threads in (2,12):
            stem=directory/f'{name}-t{threads}'
            command([probe,media,threads,0,'legacy',str(stem)+'-off',1,0,1])
            expected=[line.split(',') for line in pathlib.Path(str(stem)+'-off.sha256').read_text().splitlines()]
            assert [r[-1] for r in expected]==hashes, name
            assert all(abs(int(r[0])-round(float(f['pts_time'])*1e6))<=1 for r,f in zip(expected,frames))
            for cycles in (1,3):
                target=str(stem)+f'-on-{cycles}'
                command([probe,media,threads,0,'async',target,cycles,0,1],env=enabled)
                got=[line.split(',') for line in pathlib.Path(target+'.sha256').read_text().splitlines()]
                reference=expected*cycles
                assert len(got)==len(reference)
                assert all(a[:2]+a[3:]==b[:2]+b[3:] for a,b in zip(reference,got)), (name,threads,cycles)
                if codec=='h264':assert got==reference, 'HEVC opt-in changed H.264 output'
                comparisons.append(dict(threads=threads,cycles=cycles,frames=len(got),exact_pixels_pts_order_metadata=True,
                                        trigger_dts_changes=sum(a[2]!=b[2] for a,b in zip(reference,got))))
        # Stress lifecycle with a deep, open reference hierarchy.
        lifecycle=[]
        if bframes==7 and opened and pyramid and depth==8:
            # pause asserts that just the first IDR packet yields a picture.
            # That is the HEVC opt-in contract; H.264 still obeys SPS reorder.
            modes=('pause','gate','flush','error','close','api') if codec=='hevc' else ('gate','flush','error','close','api')
            for mode in modes:
                target=directory/(name+'-'+mode)
                command([probe,media,12,0,mode,target,1,20000 if mode in ('flush','close') else 0,1],env=enabled)
                if mode!='close':
                    got=[line.split(',') for line in target.with_suffix('.sha256').read_text().splitlines()]
                    assert len(got)==len(expected) and all(a[:2]+a[3:]==b[:2]+b[3:] for a,b in zip(expected,got)), (name,mode)
                lifecycle.append(mode)
        results.append(dict(name=name,codec=codec,bit_depth=depth,bframes=bframes,open_gop=bool(opened),
            b_pyramid=bool(pyramid),observed_max_consecutive_b=max(runs),reference_b_frames=reference_b,nal_counts=counts,
            frames=len(frames),display_types=types,source_sha256=hashlib.sha256(data).hexdigest(),
            independent_system_pixels_equal=True,comparisons=comparisons,lifecycle=lifecycle))
        (directory/'progress.json').write_text(json.dumps(dict(completed=results),indent=2)+'\n')
        print('PASS input reorder:',name,flush=True)
    result=dict(status='passed',cases=results,scope='Generated 192-frame 320x180 clips at 60fps; correctness only, not latency/capacity. pkt_dts is output-trigger packet DTS and excluded from HEVC opt-in parity.')
    (directory/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
