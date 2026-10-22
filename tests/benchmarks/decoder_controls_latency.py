#!/usr/bin/env python3
"""Local, paced service comparison with declared counts; no production SLO claim."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/integration'))
from decoder_controls_integration import ControlsHarness, require, traces, wait_admission
from support.service_harness import CACHE


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def distribution(rows, field):
    values = sorted(row[field] for row in rows)
    require(values and all(math.isfinite(x) and x >= 0 for x in values), 'invalid ' + field)
    return {'count': len(values), 'mean_ms': sum(values)/len(values),
            **{f'p{p}_ms': values[math.ceil(len(values)*p/100)-1] for p in [50, 95, 99]},
            'max_ms': values[-1]}


def start_controller(harness,directory,log):
    server=subprocess.Popen([str(harness.BIN/'naki-live'),'--port',str(harness.PORT),
                             '--run-dir',str(directory/'logs')],stdout=log,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+10
        while True:
            require(server.poll() is None,'controller exited')
            try: harness.http('/health'); return server
            except OSError:
                if time.monotonic()>deadline: raise TimeoutError('controller health')
                time.sleep(.02)
    except BaseException:
        server.terminate()
        try: server.wait(timeout=15)
        except subprocess.TimeoutExpired: server.kill();server.wait()
        raise


def stop_controller(server):
    server.terminate()
    try: code=server.wait(timeout=15)
    except subprocess.TimeoutExpired:
        server.kill();server.wait();raise TimeoutError('controller shutdown')
    require(code==0,'controller shutdown failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True,
                        help='Object: frames_per_cycle (declared), inputs [{case,path,sha256}]')
    parser.add_argument('--results', type=Path, required=True, help='New local directory')
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--include-pump', action='store_true')
    parser.add_argument('--job-timeout', type=int, default=60, help='Declared bounded deadline; increase for long paced fixtures')
    args = parser.parse_args()
    require(60 <= args.job_timeout <= 600, 'job-timeout must be 60..600 seconds')
    require(args.repeats > 0 and 2 <= args.threads <= 12, 'invalid repeats/threads')
    manifest = json.loads(args.manifest.read_text())
    count = manifest['frames_per_cycle']
    require(isinstance(count, int) and count > 80 and manifest['inputs'], 'declare more than 80 frames')
    names = [x['case'] for x in manifest['inputs']]
    require(len(set(names)) == len(names) and all(n and Path(n).name == n and n not in ['.', '..'] for n in names), 'unsafe/duplicate case')
    for entry in manifest['inputs']:
        require(sha(Path(entry['path'])) == entry['sha256'], 'fixture identity mismatch')
    subprocess.run([sys.executable,str(ROOT/'scripts/sdk-expected.py'),'verify',
                    '--prefix',str(CACHE/'install')],check=True)
    directory = args.results.resolve(); directory.mkdir(parents=True, exist_ok=False)
    harness = ControlsHarness(directory)
    binaries = {str(harness.BIN / n): sha(harness.BIN / n) for n in ['naki-live', 'naki-live-worker']}
    loaded=subprocess.check_output(['ldd',str(harness.BIN/'naki-live-worker')],text=True)
    libraries={p:sha(Path(p)) for p in re.findall(r'=> (/\S+)',loaded)}
    require(any('/libavcodec.so' in p for p in libraries),'missing loaded decoder identity')
    for path in libraries:
        if Path(path).name.startswith(('libav','libsw')):
            require(Path(path).resolve().parent==(CACHE/'install/lib').resolve(),'binary loads a different SDK')
    sources={str(p):sha(p) for p in [Path(__file__).resolve(),ROOT/'tests/integration/decoder_controls_integration.py',
                                    ROOT/'tests/support/service_harness.py',ROOT/'app/media/pipeline.c',
                                    ROOT/'app/media/pipeline_decode.c',ROOT/'app/media/pipeline_decode_queue.c',
                                    ROOT/'app/media/pipeline_internal.h',ROOT/'app/platform/wakeup.h']}
    manifest_path=CACHE/'install/share/naki/ffmpeg-build.json'
    manifest_hash=sha(manifest_path)
    cases = [(a,e,p) for a in [False,True] for p in ([False,True] if a and args.include_pump else [False]) for e in [False,True]]
    require(len(cases)*args.repeats<=24,'per-input scope exceeds reserved creation-record budget')
    plan = dict(input_manifest=manifest, manifest_sha256=sha(args.manifest), repeats=args.repeats,
                threads=args.threads, job_timeout_s=args.job_timeout, cases=cases, binaries=binaries, loaded_libraries=libraries,
                sdk_manifest_sha256=manifest_hash,sources=sources,platform=platform.platform(),
                measurement='service ingress to actual mux/flush; all frames and steady frames [32:-48]',
                controller_scope='one fresh controller per input, at most 24 creations; normal retention/admission unchanged',
                queue_sampling='HTTP snapshots, approx 1Hz decoder refresh; not continuous maxima',
                cpu='not measured', performance_acceptance=False)
    (directory/'plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    results=[]; server=None; failure=None
    try:
        with (directory/'controller.log').open('x') as log:
            for entry in manifest['inputs']:
                # Completed create outcomes are intentionally retained for 30
                # minutes in a bounded cache. Isolate fixture groups rather
                # than weakening admission or evicting production records.
                if server is not None: stop_controller(server)
                server=start_controller(harness,directory,log)
                reference=None
                for repeat in range(args.repeats):
                    # Reverse paired order on alternate repeats to expose order bias.
                    for asynchronous,early,pump in (cases if repeat%2==0 else list(reversed(cases))):
                        label=f'{entry["case"]}-r{repeat}-async{int(asynchronous)}-early{int(early)}-pump{int(pump)}'
                        config=dict(input=entry['path'],output=str(directory/(label+'.ts')),width=320,height=180,
                                    max_threads=args.threads,min_threads=2,encoder_threads=1,queue_capacity=1,
                                    decoder_async=asynchronous,decoder_auto_threads=False,decoder_early_output=early,
                                    decoder_pump=pump,audio_copy=False,frame_log=True,frame_log_every=1,pace=True)
                        (directory/(label+'-request.json')).write_text(json.dumps(config,indent=2)+'\n')
                        wait_admission(harness); harness.start(config)
                        began=time.monotonic(); samples=[]
                        while True:
                            state=harness.http()
                            samples.append(dict(observed_s=time.monotonic()-began,state=state['state'],
                                                decoded=state.get('frames_decoded',0),written=state.get('packets_out',0),
                                                oldest_ms=state.get('overload_oldest_ingress_age_ms'),
                                                decoder_queues=state.get('auto'),snapshot_age_ms=state.get('auto_stats_age_ms')))
                            if state['state'] in ['completed','completed_partial','failed','stopped']: break
                            require(time.monotonic()-began<args.job_timeout,'job timed out')
                            time.sleep(.05)
                        terminal_elapsed=time.monotonic()-began
                        (directory/(label+'-state.json')).write_text(json.dumps(state,indent=2)+'\n')
                        (directory/(label+'-queues.json')).write_text(json.dumps(samples,indent=2)+'\n')
                        require(state['state']=='completed','unsuccessful terminal state')
                        require(state['frames_decoded']==state['frames_filtered']==state['packets_out']==count,'declared frame count mismatch')
                        require(state['decoder_async']==int(asynchronous) and state['decoder_pump']==int(pump) and
                                state['decoder_auto_threads']==0 and state['decoder_early_output_requested']==int(early),'wrong controls')
                        observed=state['decoder_early_output']
                        require(observed['available'] and observed['selected_frames']==count and
                                sum(observed['selected_by_mode'].values())==count,'invalid complete codec observations')
                        require(early or observed['early_selected_frames']==0,'disabled policy selected early frames')
                        expected_path=entry.get('expected_early_path')
                        if early and expected_path:
                            if expected_path=='fallback':require(observed['early_selected_frames']==0,'fallback unexpectedly selected early frames')
                            else:require(observed['selected_by_mode'][expected_path]>0,'expected early path was not exercised')
                        rows=traces(state,count)
                        ordered=[(r['pts_us'],r['input_sequence'],r['decoded_picture_type'],r['decoded_key']) for r in rows]
                        require(set(r['input_sequence'] for r in rows)==set(range(count)),'missing/duplicate source IDs')
                        require([r['pts_us'] for r in rows]==sorted(r['pts_us'] for r in rows),'unordered display')
                        if reference is None: reference=ordered
                        require(ordered==reference,'cross-configuration display mismatch')
                        output=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
                            '-count_frames','-show_entries','stream=nb_read_frames','-of','json',config['output']],text=True))['streams']
                        require(len(output)==1 and int(output[0]['nb_read_frames'])==count,'decoded output count mismatch')
                        fields=['ingress_to_mux_ms','input_queue_ms','decoder_submit_to_receive_ms','decode_ms']
                        result=dict(case=label,input=entry['case'],repeat=repeat,asynchronous=asynchronous,early=early,pump=pump,
                                    frames=count,post_accept_to_terminal_s=terminal_elapsed,codec_observation=observed,
                                    all={f:distribution(rows,f) for f in fields},
                                    steady={f:distribution(rows[32:-48],f) for f in fields})
                        results.append(result)
                        (directory/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                        print(label, result['steady']['ingress_to_mux_ms'],flush=True)
            require(all(sha(Path(e['path']))==e['sha256'] for e in manifest['inputs']),'input changed')
            require(all(sha(Path(p))==digest for p,digest in binaries.items()),'binary changed')
            require(all(sha(Path(p))==digest for p,digest in libraries.items()),'loaded library changed')
            require(all(sha(Path(p))==digest for p,digest in sources.items()),'measurement source changed')
            require(sha(manifest_path)==manifest_hash,'SDK manifest changed')
    except BaseException as error:
        failure=f'{type(error).__name__}: {error}'
        raise
    finally:
        if server is not None:
            try: stop_controller(server)
            except Exception as error: failure=failure or f'{type(error).__name__}: {error}'
        status=dict(status='failed' if failure else 'passed',error=failure,cases_completed=len(results),
                    cases_expected=len(manifest['inputs'])*len(cases)*args.repeats,performance_acceptance=False)
        (directory/'summary.json').write_text(json.dumps(status,indent=2)+'\n')
    require(failure is None and len(results)==status['cases_expected'],'incomplete benchmark')
    print(directory)


if __name__=='__main__':
    main()
