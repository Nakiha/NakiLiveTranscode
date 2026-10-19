#!/usr/bin/env python3
"""Finite RTMP pull/transcode/push measurement, with frame target and capture checks."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import collections
import contextlib
import fractions
import json
import os
import platform
import signal
import shutil
import subprocess
import tempfile
import time
from benchmarks.service_capacity import digest, process_sample
from support.service_harness import ServiceHarness, PROBE, CACHE, new_run
from support.vvc_structure import decode_frames
from integration.frame_policy_integration import check_vvc_intra_groups, refresh_packets


def probe(path, directory, name):
    command = [str(PROBE), '-v', 'error', '-threads', '2', '-count_frames',
               '-show_entries', 'stream=codec_type,codec_name,pix_fmt,width,height,nb_read_frames,r_frame_rate,duration:format=duration,size',
               '-of', 'json', str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    (directory / (name + '.stderr')).write_text(result.stderr)
    (directory / (name + '.json')).write_text(result.stdout)
    if result.returncode or result.stderr.strip():
        raise RuntimeError(f'{name} failed independent decode: {result.stderr}')
    return json.loads(result.stdout)


def listening(port):
    # Inspect the listener without consuming its sole RTMP connection.
    return any(line.split()[1].endswith(f':{port:04X}') and line.split()[3] == '0A'
               for line in _pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:])


def wait_listener(process, port):
    deadline = time.monotonic() + 10
    while not listening(port):
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f'RTMP listener failed on port {port}')
        time.sleep(.02)


def measure(original, directory, seconds, mode, expected_frames):
    client = ServiceHarness()
    source_port, capture_port = client.port(), client.port()
    while capture_port == source_port:
        capture_port = client.port()
    source_url = f'rtmp://127.0.0.1:{source_port}/live/input'
    capture_url = f'rtmp://127.0.0.1:{capture_port}/live/output'
    captured = directory / 'captured.flv'
    config = dict(original, input=source_url, output=capture_url, output_format='flv', pace=False)
    if 'outputs' in config:
        raise ValueError('RTMP benchmark measures one fixed output')
    (directory / 'resolved-config.json').write_text(json.dumps(config, indent=2) + '\n')
    commands = {
        'sender': [str(client.BIN / 'naki-live-transport-fixture'), 'serve-hold', source_url,
                   original['input'], 'flv' if mode == 'paced' else 'flv-fast', str(min(86400, int(seconds) + 30))],
        'receiver': [str(CACHE / 'install/bin/ffmpeg'), '-nostdin', '-v', 'warning',
                     '-copyts', '-listen', '1', '-rw_timeout', '15000000', '-i', capture_url,
                     '-map', '0', '-c', 'copy', '-f', 'flv', str(captured)],
    }
    if original.get('rtmp_tcp_nodelay'):
        commands['sender'].append('1')
        position=commands['receiver'].index('-i')
        commands['receiver'][position:position]=['-tcp_nodelay','1']
    processes, returncodes = {}, {}
    metrics = ('startup', 'input_queue', 'decode', 'filter', 'encode', 'encode_submit', 'encode_wait',
               'mux', 'ingress_to_mux', 'frame_types', 'i_frame_stages', 'output_key_stages', 'logging', 'auto', 'video_rate')
    metrics += ('network_to_mux','network_read')
    with contextlib.ExitStack() as stack:
        state_dir = stack.enter_context(tempfile.TemporaryDirectory(prefix='rtmp-bench-state-'))
        commands['service'] = [str(client.BIN / 'naki-live'), '--port', str(client.PORT),
                               '--state-dir', state_dir, '--run-dir', str(directory / 'logs'), '--status-interval', '1']
        (directory / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        samples = stack.enter_context((directory / 'samples.jsonl').open('w'))
        status_history = stack.enter_context((directory / 'status.jsonl').open('w'))
        try:
            for name in ('service', 'sender', 'receiver'):
                log = stack.enter_context((directory / (name + '.log')).open('w'))
                processes[name] = subprocess.Popen(commands[name], stdout=log, stderr=log)
            wait_listener(processes['sender'], source_port)
            wait_listener(processes['receiver'], capture_port)
            deadline = time.monotonic() + 10
            while True:
                if processes['service'].poll() is not None:
                    raise RuntimeError('Controller exited during startup')
                try:
                    if client.http('/health')['admission']['open']:
                        break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError('Controller health timeout')
                time.sleep(.03)
            previous, totals = {}, {}
            for name, process in processes.items():
                previous[name] = {(p['pid'], p['start_ticks']): p['cpu_ticks'] for p in process_sample(process.pid)}
                totals[name] = dict(cpu_ticks=0, peak_rss_bytes=0, peak_threads=0, peak_fds=0)
            pending_progress = collections.deque()
            steady_first = steady_last = None
            began = time.monotonic()
            accepted = client.start(config)
            while True:
                state = client.http()
                elapsed = time.monotonic() - began
                status_history.write(json.dumps(dict(elapsed_s=elapsed, status=state)) + '\n')
                status_history.flush()
                scoped = {}
                for name, process in processes.items():
                    rows = process_sample(process.pid)
                    scoped[name] = rows
                    for p in rows:
                        key = p['pid'], p['start_ticks']
                        totals[name]['cpu_ticks'] += max(0, p['cpu_ticks'] - previous[name].get(key, 0))
                        previous[name][key] = p['cpu_ticks']
                    for metric, field in (('rss_bytes', 'peak_rss_bytes'), ('threads', 'peak_threads'), ('fds', 'peak_fds')):
                        totals[name][field] = max(totals[name][field], sum(p[metric] for p in rows))
                samples.write(json.dumps(dict(elapsed_s=elapsed, processes=scoped)) + '\n')
                samples.flush()
                progress = dict(elapsed_s=elapsed, packets_out=state.get('packets_out', 0))
                pending_progress.append(progress)
                # Only retain the last five seconds, plus endpoints for the middle window.
                while pending_progress and pending_progress[0]['elapsed_s'] <= elapsed - 5:
                    candidate = pending_progress.popleft()
                    if candidate['elapsed_s'] >= 5 and candidate['packets_out']:
                        if steady_first is None:
                            steady_first = candidate
                        steady_last = candidate
                if state.get('packets_out', 0) >= expected_frames:
                    # Stop while source remains connected, before the finite-file tail.
                    # Decoder/encoder lookahead otherwise needs EOF to release that tail.
                    if state['state'] not in ('running', 'starting'):
                        raise RuntimeError('Service ended before explicit frame-target stop')
                    client.http('/v1/job/stop', data={'job_id': accepted['job_id']}, code=202)
                    state = client.finished('stopped', timeout=25)
                    elapsed = time.monotonic() - began
                    # Capture CPU accumulated during controlled finalization as well.
                    for name, process in processes.items():
                        for p in process_sample(process.pid):
                            key = p['pid'], p['start_ticks']
                            totals[name]['cpu_ticks'] += max(0, p['cpu_ticks']-previous[name].get(key,0))
                            previous[name][key] = p['cpu_ticks']
                    break
                if state['state'] in ('completed', 'stopped', 'failed', 'completed_partial'):
                    break
                if elapsed >= seconds:
                    client.http('/v1/job/stop', data={'job_id': accepted['job_id']}, code=202)
                    state = client.finished('stopped', timeout=25)
                    (directory / 'final-state.json').write_text(json.dumps(state, indent=2) + '\n')
                    raise RuntimeError('Observation limit reached before video frame target')
                if processes['receiver'].poll() is not None:
                    raise RuntimeError('RTMP receiver exited before service completion')
                time.sleep(.25)
            (directory / 'final-state.json').write_text(json.dumps(state, indent=2) + '\n')
            if state['state'] != 'stopped' or state.get('packets_out', 0) < expected_frames:
                raise RuntimeError('RTMP workload failed: ' + str(state.get('error')))
            if processes['sender'].poll() is not None:
                raise RuntimeError('Source exited before controlled stop')
            processes['sender'].terminate()
            returncodes['sender'] = processes['sender'].wait(timeout=5)
            returncodes['receiver'] = processes['receiver'].wait(timeout=20)
            if returncodes['receiver']:
                raise RuntimeError('Receiver failed; see its log')
            # Includes probe/startup and explicit stop/finalization, excludes decode verification.
            progress_fps = ((steady_last['packets_out'] - steady_first['packets_out']) /
                            (steady_last['elapsed_s'] - steady_first['elapsed_s'])) if steady_first and steady_last['elapsed_s'] > steady_first['elapsed_s'] else None
            for total in totals.values():
                total['sampled_cpu_s'] = total.pop('cpu_ticks') / os.sysconf('SC_CLK_TCK')
                total['sampled_average_cpu_cores'] = total['sampled_cpu_s'] / elapsed
            return dict(elapsed_s=elapsed, stop_reason='video_frame_target_then_api_stop', frame_target=expected_frames, packets_out=state['packets_out'],
                        decoded_frames=state['frames_decoded'], fps_including_startup_and_stop=state['packets_out'] / elapsed,
                        progress_fps_excluding_first_and_last_5s=progress_fps, resources=totals,
                        progress_window=dict(first=steady_first,last=steady_last),
                        service_metrics={key: state.get(key) for key in metrics},
                        frames_selected=state.get('frames_selected'), frames_filtered=state.get('frames_filtered'),
                        frames_dropped_rate=state.get('frames_dropped_rate'), frames_dropped_overload=state.get('frames_dropped_overload'),
                        intra_requested=state.get('intra_requested'), intra_confirmed=state.get('intra_confirmed'),
                        audio_packets_out=state.get('audio_packets_out'),
                        audio_dropped=state.get('audio_dropped'), captured=str(captured))
        finally:
            for name in ('service', 'sender', 'receiver'):
                process = processes.get(name)
                if process is None:
                    continue
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=25 if name == 'service' else 5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                returncodes[name] = process.returncode
            (directory / 'returncodes.json').write_text(json.dumps(returncodes, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=_pathlib.Path)
    parser.add_argument('--config-sha256', required=True)
    parser.add_argument('--seconds', type=float, default=120)
    parser.add_argument('--repeats', type=int, choices=range(1, 11), default=3)
    parser.add_argument('--mode', choices=('paced', 'throughput'), default='paced')
    parser.add_argument('--input-fps', type=int)
    parser.add_argument('--input-gop', type=int)
    parser.add_argument('--save-output-dir', type=_pathlib.Path)
    parser.add_argument('--audit-burst-bits', type=int, default=55000)
    parser.add_argument('--latency-observe-only',action='store_true')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 259200 or digest(args.config) != args.config_sha256:
        parser.error('Invalid duration or changed configuration')
    config = json.loads(args.config.read_text())
    media = _pathlib.Path(config['input']).expanduser()
    media = (media if media.is_absolute() else args.config.parent / media).resolve()
    config['input'] = str(media)
    if not media.is_file() or 'outputs' in config:
        parser.error('Requires a finite local H.264/HEVC file and one output')
    saved = args.save_output_dir.resolve() if args.save_output_dir else None
    if saved:
        root = _pathlib.Path(__file__).resolve().parents[2] / 'out'
        if not saved.is_relative_to(root.resolve()) or saved.exists():
            parser.error('--save-output-dir must be a new directory under out/')
        saved.mkdir(parents=True, exist_ok=False)
    run = new_run('rtmp-bench-')
    evidence = dict(status='running', mode=args.mode, input=str(media), input_sha256=digest(media),
                    config_sha256=args.config_sha256, results=[],
                    topology='Loopback RTMP source listener -> service pull/transcode/push -> capture listener',
                    boundary='Ingress/handler to actual service mux/flush, not source-to-screen; CPU/RSS sampled per process tree, RSS sums shared pages. Sender/receiver reported separately. Short-lived processes may be missed.',
                    frame_log=config.get('frame_log', False), maximum_seconds=args.seconds)
    cpu_model = next((line.split(':',1)[1].strip() for line in _pathlib.Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')), 'unknown')
    evidence['environment'] = dict(kernel=platform.platform(), cpu_model=cpu_model,
                                   vvenc_busy_wait_time=os.environ.get('BUSY_WAIT_TIME','default:1ms'),
                                   cpu_affinity=sorted(os.sched_getaffinity(0)),
                                   meminfo=_pathlib.Path('/proc/meminfo').read_text(),
                                   source_sender='Pinned SDK raw FLV RTMP fixture; no encoding during measurement')
    try:
        source = probe(media, run, 'source-probe')
        video = next(s for s in source['streams'] if s['codec_type'] == 'video')
        if video['codec_name'] not in ('h264', 'hevc'):
            raise ValueError('Input must be H.264 or HEVC')
        count = int(video['nb_read_frames'])
        if count > 250000:
            raise ValueError('This finite benchmark caps independent frame evidence at 250000 source frames')
        reference = decode_frames(PROBE, media)
        (run / 'source-frames.json').write_text(json.dumps(reference) + '\n')
        if len(reference) != count:
            raise RuntimeError('Source decode count mismatch')
        source_fps = fractions.Fraction(video['r_frame_rate'])
        source_times = [float(f['pts_time']) for f in reference]
        if args.input_fps:
            if source_fps != args.input_fps or any(abs(b-a-1/args.input_fps) > .0011 for a,b in zip(source_times,source_times[1:])):
                raise ValueError('Source cadence differs from requested input FPS')
        if args.input_gop:
            if [i for i,f in enumerate(reference) if f['pict_type'] == 'I'] != list(range(0,count,args.input_gop)):
                raise ValueError('Source decoded I interval differs from requested GOP')
        divisor = 1
        if config.get('output_fps'):
            ratio = source_fps / config['output_fps']
            if ratio.denominator != 1 or not 1 <= ratio <= 256:
                raise ValueError('Output FPS requires an integer input/output ratio')
            if config.get('intra',{}).get('mode') != 'follow_source' or config.get('frame_selection',{}).get('mode','all') != 'all' or config.get('sei',{}).get('types'):
                raise ValueError('Decimation benchmark requires follow_source, no overload dropping or selected SEI')
            divisor = int(ratio)
        selected = [f for i,f in enumerate(reference) if i % divisor == 0 or f['pict_type'] == 'I']
        # Leave one nominal second for decoder/encoder reordering at the end.
        # Actual processed counts and media duration are reported, never the full source duration.
        guard = max(32, round(float(fractions.Fraction(video['r_frame_rate']))))
        target = sum(i % divisor == 0 or reference[i]['pict_type'] == 'I' for i in range(max(0,count-guard)))
        if target < 1:
            raise ValueError('Source too short for a live measurement plus reorder tail')
        evidence.update(source_frames=count, expected_selected_frames=len(selected), decimation_divisor=divisor,
                        input_fps=str(source_fps), input_gop=args.input_gop, tail_guard_frames=guard, frame_target=target)
        # The fixture sends FLV bytes verbatim. Remux once outside all timed runs.
        fixture = run / 'source.flv'
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-copyts', '-i', str(media), '-map', '0:v:0',
                   '-map', '0:a:0?', '-c', 'copy', '-flvflags', 'no_duration_filesize', '-f', 'flv', str(fixture)]
        (run / 'source-remux-command.json').write_text(json.dumps(command, indent=2) + '\n')
        subprocess.run(command, check=True, timeout=120, stdout=subprocess.DEVNULL,
                       stderr=(run / 'source-remux.stderr').open('w'))
        config['input'] = str(fixture)
        evidence['source_flv_sha256'] = digest(fixture)
        for repeat in range(args.repeats):
            directory = run / str(repeat + 1)
            directory.mkdir()
            row = measure(config, directory, args.seconds, args.mode, target)
            evidence['results'].append(row)
            output = probe(_pathlib.Path(row['captured']), directory, 'capture-probe')
            row['media'] = output
            out_video = next(s for s in output['streams'] if s['codec_type'] == 'video')
            if int(out_video['nb_read_frames']) != row['packets_out'] or not target <= row['packets_out'] <= row['decoded_frames'] <= count:
                raise RuntimeError('Source/service/captured decoded frame count mismatch')
            if row['frames_selected'] + row['frames_dropped_rate'] + row['frames_dropped_overload'] != row['decoded_frames']:
                raise RuntimeError('Decoded frame selection/drop accounting mismatch')
            expected_drops = sum(i % divisor != 0 and reference[i]['pict_type'] != 'I' for i in range(row['decoded_frames']))
            if row['frames_dropped_rate'] != expected_drops or row['frames_dropped_overload'] or row['packets_out'] > len(selected):
                raise RuntimeError('Rate decimation or overload loss differs from the expected selection')
            if row['intra_requested'] != row['intra_confirmed']:
                raise RuntimeError('Requested input I not confirmed')
            if (out_video['width'], out_video['height']) != (config.get('width') or video['width'], config.get('height') or video['height']):
                raise RuntimeError('Captured dimensions disagree with requested output')
            if config.get('encoder') == 'libvvenc' and out_video['codec_name'] != 'vvc':
                raise RuntimeError('Captured video is not VVC')
            if config.get('encoder') == 'libvvenc':
                expected_format = 'yuv420p' if config.get('encoder_bit_depth', 10) == 8 else 'yuv420p10le'
                if out_video['pix_fmt'] != expected_format:
                    raise RuntimeError('Independently decoded VVC precision differs from requested encoder_bit_depth')
            decoded = decode_frames(PROBE, row['captured'])
            (directory / 'capture-frames.json').write_text(json.dumps(decoded) + '\n')
            # Controlled stop may omit up to one reordered short group. Validate
            # all source associations, and exact cadence/3B for the prefix before it.
            prefix = decoded[:-4]
            if not prefix:
                raise RuntimeError('Insufficient captured frames for structure validation')
            if any(abs(float(a['pts_time'])-float(b['pts_time'])) > .0011 for a,b in zip(prefix,selected)):
                raise RuntimeError('Retained source PTS changed or output cadence is wrong')
            expected_times = {round(float(f['pts_time'])*1000) for f in selected}
            if any(round(float(f['pts_time'])*1000) not in expected_times for f in decoded):
                raise RuntimeError('Captured frame has no selected source timestamp')
            if config.get('intra',{}).get('mode') == 'follow_source':
                if [f['pict_type']=='I' for f in prefix] != [f['pict_type']=='I' for f in selected[:len(prefix)]]:
                    raise RuntimeError('Actual output I pictures do not follow source I')
                if config.get('encoder') == 'libvvenc':
                    # Validate complete intervals between I pictures; last interval
                    # may be cut by cancellation and is checked for <=3 B runs.
                    types = ''.join(f['pict_type'] for f in prefix)
                    last_i = types.rfind('I')
                    if last_i:
                        check_vvc_intra_groups(prefix[:last_i])
                    if 'BBBB' in types:
                        raise RuntimeError('Captured VVC has more than 3 consecutive B pictures')
                    if refresh_packets(_pathlib.Path(row['captured']), 'libvvenc') != [0]:
                        raise RuntimeError('Follow-source ordinary I unexpectedly introduced refresh pictures')
            row['structure'] = dict(validated_prefix_frames=len(prefix), cancellation_tail_frames=len(decoded)-len(prefix),
                                    input_i_count=sum(f['pict_type']=='I' for f in reference),
                                    output_i_pts=[f['pts_time'] for f in decoded if f['pict_type']=='I'],
                                    output_types=''.join(f['pict_type'] for f in decoded))
            if any(s['codec_type'] == 'audio' for s in source['streams']) and config.get('audio_copy', True):
                if not any(s['codec_type'] == 'audio' and int(s.get('nb_read_frames', 0)) > 0 for s in output['streams']):
                    raise RuntimeError('Captured audio missing')
            row['output_sha256'] = digest(_pathlib.Path(row['captured']))
            if config.get('network_trace'):
                audit_command=[_sys.executable,str(_pathlib.Path(__file__).resolve().parents[2]/'scripts/audit-live-budget.py'),
                               str(directory),'--maxrate',str(config['maxrate']),'--burst-bits',str(args.audit_burst_bits)]
                if args.latency_observe_only:audit_command += ['--latency-observe-only']
                (directory/'audit-command.json').write_text(json.dumps(audit_command,indent=2)+'\n')
                with (directory/'audit.log').open('w') as log:
                    subprocess.run(audit_command,stdout=log,stderr=log,check=True,timeout=120)
                audit=json.loads((directory/'budget-audit.json').read_text())
                row['target_audit']={k:v for k,v in audit.items() if k not in ('output_wall_rolling_1s','output_dts_rolling_1s','dual_buckets')}
                row['target_audit']['budget_summary']={k:v for k,v in audit['dual_buckets'].items() if k!='rows'}
                evidence['target_conformance']='passed' if all(r.get('target_audit',{}).get('conformance_status')=='passed' for r in evidence['results']) else 'failed'
            if config.get('codec_trace'):
                phase_command=[_sys.executable,str(_pathlib.Path(__file__).resolve().parents[2]/'scripts/audit-codec-phases.py'),
                               str(directory),'--output',str(directory/'codec-phase-audit.json')]
                (directory/'codec-phase-command.json').write_text(json.dumps(phase_command,indent=2)+'\n')
                with (directory/'codec-phase-audit.log').open('w') as log:
                    subprocess.run(phase_command,stdout=log,stderr=log,check=True,timeout=120)
                row['codec_phase_audit']=str(directory/'codec-phase-audit.json')
            if saved:
                output_copy = saved / f'output-{repeat + 1}.flv'
                shutil.copyfile(row['captured'], output_copy)
                if digest(output_copy) != row['output_sha256']:
                    raise RuntimeError('Saved output differs from RTMP capture')
                row['saved_output'] = str(output_copy)
            row['nominal_video_seconds'] = int(out_video['nb_read_frames']) / float(source_fps / divisor)
            row['nominal_video_seconds_per_wall_second'] = row['nominal_video_seconds'] / row['elapsed_s']
            (run / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
            print(f'{args.mode} repeat={repeat + 1} frames={row["packets_out"]} elapsed={row["elapsed_s"]:.3f}s fps={row["fps_including_startup_and_stop"]:.2f}', flush=True)
        if digest(media) != evidence['input_sha256'] or digest(args.config) != args.config_sha256:
            raise RuntimeError('Input/config changed during measurement')
        evidence['status'] = 'passed'
        (run / 'COMPLETE.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print('PASS RTMP measurement:', run, flush=True)
    except BaseException as error:
        evidence.update(status='failed', error=str(error))
        raise
    finally:
        (run / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
        if saved:
            (saved / 'manifest.json').write_text(json.dumps(dict(benchmark_run=str(run),
                status=evidence['status'], boundary='Verified compressed RTMP capture copied after timing; no re-encoding',
                outputs=[dict(path=r['saved_output'], sha256=r['output_sha256']) for r in evidence['results'] if 'saved_output' in r]), indent=2) + '\n')


if __name__ == '__main__':
    main()
