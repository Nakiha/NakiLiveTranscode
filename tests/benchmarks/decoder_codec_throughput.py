#!/usr/bin/env python3
"""Bounded final-SDK async throughput subset; separate correctness and timing.
Uses load.c probes built against two immutable SDKs. Inputs must have one video
AU per declared frame and a manifest [{path,frames,sha256}]. Use an eligible
early-output fixture: the candidate must actually select early frames. Not service capacity.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
TOOLS=ROOT/'experiments/decoder-20261016/ffmpeg-7.0.3/tools/early-output'
sys.path.insert(0,str(TOOLS))
from evidence import require,sha256
from run_representatives import verify_identity


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--identity',type=Path,required=True)
    ap.add_argument('--manifest',type=Path,required=True)
    ap.add_argument('--results',type=Path,required=True)
    ap.add_argument('--candidate-prefix',type=Path,required=True)
    args=ap.parse_args()
    subprocess.run([sys.executable,str(ROOT/'scripts/sdk-expected.py'),'verify','--prefix',str(args.candidate_prefix)],check=True)
    identity=json.loads(args.identity.read_text());builds=identity['builds']
    require(identity['probe_source_sha256']==sha256(TOOLS/'load.c'),'probe source differs from pinned load tool')
    receipt=args.candidate_prefix/'share/naki/ffmpeg-build.json';receipt_hash=sha256(receipt)
    sdk=json.loads(receipt.read_text())
    fixtures=json.loads(args.manifest.read_text());require(fixtures,'empty fixtures')
    args.results.mkdir(parents=True,exist_ok=False)
    manifest_hash=sha256(args.manifest);source_hash=sha256(Path(__file__));rows=[]
    def run(build,file,expected,threads,early,workload,replays,label,phase=None):
        b=builds[build];verify_identity(b);prefix=args.results/label
        command=[b['binary'],str(file),str(threads),'0','async',workload,'1',str(prefix),str(replays),str(expected),str(early)]
        env=dict(os.environ)
        for key in ['LD_LIBRARY_PATH','LD_PRELOAD','EARLY_OUTPUT','FFMPEG_LIVE_IDR_EARLY','FFMPEG_LIVE_POC_EARLY']:env.pop(key,None)
        with prefix.with_suffix('.log').open('x') as log:
            code=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=150).returncode
        verify_identity(b)
        log=prefix.with_suffix('.log').read_text();loaded=re.findall(r'^runtime_avcodec=(.+)$',log,re.M)
        require(code==0 and re.findall(r'^terminal_return=(-?\d+)$',log,re.M)==['0'],'failed process '+label)
        require(len(loaded)==1 and str(Path(loaded[0]).resolve()) in b['libraries'],'wrong loaded SDK')
        if build=='candidate':require(Path(loaded[0]).resolve().parent==(args.candidate_prefix/'lib').resolve(),'candidate outside verified SDK')
        result=json.loads(prefix.with_suffix('.json').read_text())
        data=list(csv.DictReader(prefix.with_suffix('.csv').open()));total=expected*replays
        require(len(data)==result['frames']==total,'wrong declared output count')
        sources=[int(r['sequence']) for r in data]
        require(sorted(sources)==list(range(total)),'missing/duplicate source ID')
        require([int(r['pts']) for r in data]==sorted(int(r['pts']) for r in data),'out-of-order display')
        if build=='candidate':
            observed=json.loads(prefix.with_suffix('.early.json').read_text())
            require(sum(observed['frames'])==observed['pictures']==total,'incomplete codec observations')
            require(sum(observed['decisions'])==total,'incomplete AU decisions')
            require(sum(observed['frames'][1:])>0 if early else not sum(observed['frames'][1:]),'wrong exercised policy')
            result['codec_observation']=observed
        digest=list(csv.reader(prefix.with_suffix('.sha256').read_text().splitlines())) if workload=='verify' else None
        if digest is not None:require(len(digest)==total and all(len(r)==14 and re.fullmatch('[0-9a-f]{64}',r[13]) for r in digest),'incomplete correctness hashes')
        row=dict(case=label,build=build,threads=threads,early=early,workload=workload,phase=phase or ('correctness' if workload=='verify' else 'timed'),expected_frames=total,
                 command=command,result=result,files={s:sha256(prefix.with_suffix(s)) for s in ['.csv','.json','.log']})
        rows.append(row);(args.results/'runs.json').write_text(json.dumps(rows,indent=2)+'\n')
        return digest,sources
    try:
        for fixture in fixtures:
            file=Path(fixture['path']);expected=fixture['frames']
            require(expected>=96 and sha256(file)==fixture['sha256'],'bad declared fixture')
            for threads in [2,12]:
                oracle,ordered=run('baseline',file,expected,threads,0,'verify',3,f'{file.stem}-t{threads}-oracle')
                for early in [0,1]:
                    label=f'{file.stem}-t{threads}-e{early}'
                    digest,sources=run('candidate',file,expected,threads,early,'verify',3,label+'-check')
                    require(sources==ordered and all(x[:2]+x[3:]==y[:2]+y[3:] for x,y in zip(oracle,digest)),'pixel/metadata/source display mismatch')
                    for build in builds:run(build,file,expected,threads,early,'perf',3,label+'-warmup-'+build,phase='warmup')
                    # The phase, not observed length or a filename pattern, selects timed runs.
                    for repeat in range(3):
                        for build in (['baseline','candidate'] if repeat%2==0 else ['candidate','baseline']):
                            run(build,file,expected,threads,early,'perf',10,label+f'-r{repeat}-'+build)
                    print(label,'PASS',flush=True)
            require(sha256(file)==fixture['sha256'],'fixture changed')
        groups={}
        for row in rows:
            if row['phase']!='timed':continue
            key=row['command'][1],row['threads'],row['early'],row['build']
            groups.setdefault(key,[]).append(row['result'])
        require(len(groups)==len(fixtures)*8,'missing planned timed groups')
        medians=[]
        for key,items in groups.items():
            require(len(items)==3,'missing timed repetitions')
            medians.append(dict(input=Path(key[0]).name,threads=key[1],early=key[2],build=key[3],
                fps=statistics.median(r['frames']*1e6/r['elapsed_us'] for r in items),
                cpu_us_per_frame=statistics.median(r['cpu_us']/r['frames'] for r in items),
                cpu_cores=statistics.median(r['cpu_us']/r['elapsed_us'] for r in items)))
        require(sha256(receipt)==receipt_hash,'SDK receipt changed')
        require(sha256(args.manifest)==manifest_hash and sha256(Path(__file__))==source_hash,'measurement inputs changed')
        summary=dict(status='passed',scope='final SDK; async 2/12 threads; 10 continuous replays; 3 timed repetitions',
            production_capacity_accepted=False,candidate_sdk_identity=sdk['identity'],candidate_sdk_manifest_sha256=receipt_hash,identity=identity,manifest=fixtures,manifest_sha256=manifest_hash,
            measurement_source_sha256=source_hash,medians=medians)
        (args.results/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    except BaseException as error:
        (args.results/'FAILED.json').write_text(json.dumps({'error':str(error),'completed_runs':len(rows)},indent=2)+'\n');raise

if __name__=='__main__':main()
