#!/usr/bin/env python3
"""Local-file workload: per-run media, service metrics and sampled process resources."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import copy
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import tempfile
import time
from support.service_harness import ServiceHarness, PROBE, new_run


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def process_sample(pid, seen=None):
    """Read owned descendants. Identity is (pid,starttime); never signal a sampled PID."""
    seen = set() if seen is None else seen
    if pid in seen:
        return []
    seen.add(pid)
    directory = pathlib.Path('/proc') / str(pid)
    try:
        fields = (directory / 'stat').read_text().rsplit(') ', 1)[1].split()
        row = dict(pid=pid, start_ticks=int(fields[19]), cpu_ticks=int(fields[11]) + int(fields[12]),
                   rss_bytes=int(fields[21]) * os.sysconf('SC_PAGE_SIZE'),
                   threads=int(fields[17]), fds=len(list((directory / 'fd').iterdir())))
        children = [int(n) for n in (directory / 'task' / str(pid) / 'children').read_text().split()]
    except (FileNotFoundError, ProcessLookupError):
        return []
    rows = [row]
    for child in children:
        rows.extend(process_sample(child, seen))
    return rows


def resolved_config(original, directory, mode):
    config = copy.deepcopy(original)
    config['input'] = str(pathlib.Path(config['input']).expanduser().resolve())
    config['pace'] = mode == 'paced'
    outputs = config.get('outputs', [config])
    if not isinstance(outputs, list) or not 1 <= len(outputs) <= 4:
        raise ValueError('Expected one to four fixed outputs')
    for index, output in enumerate(outputs):
        fmt = output.get('output_format', config.get('output_format', 'mpegts'))
        extension = {'mpegts': 'ts', 'mp4': 'mp4', 'flv': 'flv'}[fmt]
        output['output'] = str(directory / f'output-{index}.{extension}')
    return config


def measure(config, directory, seconds):
    client = ServiceHarness()
    server = None
    with tempfile.TemporaryDirectory(prefix='bench-state-') as state_dir, \
         (directory / 'controller.stdout').open('w') as log, \
         (directory / 'controller.stderr').open('w') as error, \
         (directory / 'samples.jsonl').open('w') as observations:
        try:
            server = subprocess.Popen([str(client.BIN / 'naki-live'), '--port', str(client.PORT),
                '--run-dir', str(directory / 'logs'), '--state-dir', state_dir,
                '--status-interval', '1'], stdout=log, stderr=error)
            deadline = time.monotonic() + 10
            while True:
                if server.poll() is not None:
                    raise RuntimeError('Benchmark controller exited during startup')
                try:
                    if client.http('/health')['admission']['open']:
                        break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError('Benchmark controller did not become ready')
                time.sleep(.05)
            baseline = process_sample(server.pid)
            previous = {(p['pid'], p['start_ticks']): p['cpu_ticks'] for p in baseline}
            cpu_ticks = peak_rss = peak_threads = peak_fds = sample_count = 0
            def sample(elapsed, state):
                nonlocal previous, cpu_ticks, peak_rss, peak_threads, peak_fds, sample_count
                processes = process_sample(server.pid)
                current = {}
                for p in processes:
                    key = p['pid'], p['start_ticks']
                    cpu_ticks += max(0, p['cpu_ticks'] - previous.get(key, 0))
                    current[key] = p['cpu_ticks']
                previous = current
                peak_rss = max(peak_rss, sum(p['rss_bytes'] for p in processes))
                peak_threads = max(peak_threads, sum(p['threads'] for p in processes))
                peak_fds = max(peak_fds, sum(p['fds'] for p in processes))
                sample_count += 1
                observations.write(json.dumps(dict(elapsed_s=elapsed, processes=processes,
                    packets_out=state.get('packets_out', 0), state=state['state'])) + '\n')
                observations.flush()
            began = time.monotonic()
            accepted = client.start(config)
            reason = 'eof'
            while True:
                state = client.http()
                elapsed = time.monotonic() - began
                sample(elapsed, state)
                if state['state'] in ('completed', 'completed_partial', 'failed', 'stopped'):
                    break
                if elapsed >= seconds:
                    reason = 'observation_limit'
                    client.http('/v1/job/stop', data={'job_id': accepted['job_id']}, code=202)
                    state = client.finished('stopped', timeout=20)
                    break
                time.sleep(.25)
            elapsed = time.monotonic() - began
            sample(elapsed, state)
            if state['state'] not in ('completed', 'stopped') or not state.get('packets_out', 0):
                raise RuntimeError('Workload failed or produced no video: ' + json.dumps(state))
            (directory / 'final-state.json').write_text(json.dumps(state, indent=2) + '\n')
            cpu_s = cpu_ticks / os.sysconf('SC_CLK_TCK')
            output_states = state.get('outputs') or [state]
            if any(o['state'] not in ('completed', 'stopped') or o['packets_out'] <= 0 for o in output_states):
                raise RuntimeError('A fixed output failed or produced no video')
            metrics = ('startup', 'ingress_to_mux', 'encode', 'encode_submit', 'encode_wait',
                       'frame_types', 'i_frame_stages', 'output_key_stages')
            return dict(stop_reason=reason, elapsed_s=elapsed, decoded_frames=state['frames_decoded'],
                decoded_fps_including_startup_and_stop=state['frames_decoded'] / elapsed,
                sampled_cpu_s=cpu_s, sampled_average_cpu_cores=cpu_s / elapsed,
                sample_count=sample_count, sampled_peak_rss_bytes=peak_rss,
                sampled_peak_threads=peak_threads, sampled_peak_fds=peak_fds,
                service_metrics={name: state.get(name) for name in metrics},
                outputs=[dict(output_id=o.get('output_id'), state=o['state'], packets_out=o['packets_out'],
                              metrics={name: o.get(name) for name in metrics}) for o in output_states])
        finally:
            if server and server.poll() is None:
                server.send_signal(signal.SIGTERM)
                server.wait(timeout=25)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=pathlib.Path)
    parser.add_argument('--config-sha256', required=True)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--repeats', type=int, choices=range(1, 11), default=3)
    parser.add_argument('--mode', choices=('paced', 'throughput'), default='paced')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 259200 or digest(args.config) != args.config_sha256:
        parser.error('Invalid duration or changed benchmark configuration')
    config = json.loads(args.config.read_text())
    media = pathlib.Path(config['input']).expanduser()
    media = (media if media.is_absolute() else args.config.parent / media).resolve()
    config['input'] = str(media)
    if not media.is_file():
        parser.error('Input must be an existing local file')
    media_hash = digest(media)
    run = new_run('service-bench-')
    results = []
    evidence = dict(status='running', input=str(media), input_sha256=media_hash,
                    config_sha256=args.config_sha256, mode=args.mode, maximum_seconds=args.seconds,
                    boundary='All-inclusive file workload time; service latency ends at actual local mux/flush.',
                    resource_boundary='Sampled controller and descendants; excludes sender, verifier and kernel caches. Short-lived processes between polls can be missed.',
                    results=results)
    try:
        for repeat in range(args.repeats):
            directory = run / str(repeat + 1)
            directory.mkdir()
            resolved = resolved_config(config, directory, args.mode)
            (directory / 'resolved-config.json').write_text(json.dumps(resolved, indent=2) + '\n')
            row = measure(resolved, directory, args.seconds)
            # Verification is outside the measurement interval.
            row['media'] = []
            for index, output in enumerate(resolved.get('outputs', [resolved])):
                result = subprocess.run([str(PROBE), '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                    '-show_entries', 'stream=codec_name,width,height,nb_read_frames', '-of', 'json',
                    output['output']], check=True, capture_output=True, text=True, timeout=120)
                streams = json.loads(result.stdout)['streams']
                if not any(str(s.get('nb_read_frames', '')).isdigit() and int(s['nb_read_frames']) > 0 for s in streams):
                    raise RuntimeError('Output has no independently decoded frames')
                if row['stop_reason'] == 'eof' and int(streams[0]['nb_read_frames']) != row['outputs'][index]['packets_out']:
                    raise RuntimeError('EOF output frame count disagrees with service video packets')
                row['media'].append(dict(path=output['output'], streams=streams))
            results.append(row)
            (run / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
        if digest(args.config) != args.config_sha256 or digest(media) != media_hash:
            raise RuntimeError('Benchmark input changed during measurement')
        evidence['status'] = 'passed'
        (run / 'COMPLETE.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print('PASS service measurement:', run, flush=True)
    except BaseException as error:
        evidence.update(status='failed', error=str(error))
        raise
    finally:
        (run / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()
