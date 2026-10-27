#!/usr/bin/env python3
"""Same-input rendition isolation: fixed expected frames and real Linux children.

Every run gets new evidence paths. Success requires independent decoded frame
counts, terminal state, distinct process identities, and a complete child reap.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get('NAKI_SERVICE_BIN', ROOT / 'out/bin'))
SDK = Path(os.environ.get('NAKI_BUILD_ROOT', '/build/stable-readiness-sdk')) / 'install/bin'
ORACLE_FFMPEG = os.environ.get('NAKI_ORACLE_FFMPEG', 'ffmpeg')


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def command(args, timeout=60):
    r = subprocess.run(list(map(str, args)), capture_output=True, timeout=timeout)
    require(r.returncode == 0, f'{args}: exit {r.returncode}: {r.stderr[-4000:]!r}')
    return r.stdout


def wait(fn, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        time.sleep(.03)
    raise TimeoutError('condition deadline')


class Harness:
    def __init__(self, directory, binary=None, environment=None):
        self.run = directory
        self.binary = Path(binary) if binary else BIN
        self.environment = dict(os.environ, **(environment or {}))
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.base = f'http://127.0.0.1:{self.port}'
        self.private = tempfile.TemporaryDirectory(prefix='rp-', dir='/tmp')
        self.server = None
        self.logs = []
        self.crash_directory = None

    def api(self, path='/v1/job', body=None, expected=200):
        request = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                        headers={'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(request, timeout=6)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            result = json.load(response)
            require(response.status in (expected if isinstance(expected, tuple) else (expected,)), (path, response.status, result))
            return result

    def start(self, recovering=False):
        log = (self.run / f'controller-{len(self.logs)}.log').open('wb')
        self.logs.append(log)
        self.server = subprocess.Popen([str(self.binary / 'naki-live'), '--port', str(self.port),
                                       '--run-dir', str(self.run / 'logs'), '--state-dir', self.private.name],
                                      stdout=log, stderr=subprocess.STDOUT, env=self.environment)
        def ready():
            require(self.server.poll() is None, 'controller exited during startup')
            try:
                health = self.api('/health')
                return health if (health['readiness']['ready'] if recovering else health['admission']['open']) else None
            except OSError:
                return None
        health=wait(ready)
        self.crash_directory=Path(health['crash_report_dir'])
        return health

    def close(self):
        if self.server and self.server.poll() is None:
            self.server.send_signal(signal.SIGTERM)
            try:
                self.server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
                raise
        for log in self.logs:
            log.close()
        # On WSL the collector deliberately uses a private Linux directory if
        # the workspace cannot enforce 0700. Preserve reports before cleanup.
        if self.crash_directory and self.crash_directory.is_dir():
            reports=self.run/'crash-reports';reports.mkdir(exist_ok=True)
            for path in self.crash_directory.glob('crash-*.json'):
                target=reports/path.name
                if target.exists():require(target.read_bytes()==path.read_bytes(),'crash evidence changed')
                else:shutil.copy2(path,target)
        self.private.cleanup()


def smoke(harness, sample, frames, backend='process', count=2):
    outputs = [dict(id=f'out{i}', output=str(harness.run / f'out{i}.mp4'), output_format='mp4',
                    width=160, height=96, encoder_preset='ultrafast', audio_copy=False) for i in range(count)]
    config = dict(input=str(sample), outputs=outputs, rendition_backend=backend, pace=True,
                  max_threads=2, min_threads=2, decoder_auto_threads=False, frame_log=True,
                  timestamps={'mode': 'source_all'})
    (harness.run / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    accepted = harness.api(body=config, expected=202)
    identities = {}
    def terminal():
        state = harness.api()
        require(len(json.dumps(state).encode())<=65536,'four-rendition summary exceeds control IPC bound')
        for output in state.get('outputs', []):
            pid = output.get('rendition_pid', 0)
            if pid:
                identities[output['output_id']] = (pid, output['rendition_generation'])
        (harness.run / 'latest-state.json').write_text(json.dumps(state, indent=2) + '\n')
        return state if state['state'] in ('completed', 'completed_partial', 'failed', 'stopped') else None
    state = wait(terminal, 40)
    require(state['state'] == 'completed', {k: state.get(k) for k in ['state','error','frames_decoded','outputs_failed']})
    require(state['frames_decoded'] == frames, ('shared decode count', frames, state))
    require(len(identities) == count and len({v[0] for v in identities.values()}) == (1 if backend == 'thread' else count), identities)
    require(len(state['outputs']) == count, state)
    decoded = []
    for spec, output in zip(outputs, state['outputs']):
        require(output['output_id'] == spec['id'], output)
        require(output['packets_out'] == frames and output['frames_filtered'] == frames, output)
        require(output.get('rendition_pid') == 0, ('holder not reaped', output))
        probed = json.loads(command([SDK / 'ffprobe', '-v', 'error', '-count_frames', '-show_entries',
                                     'stream=codec_name,nb_read_frames', '-select_streams', 'v:0', '-of', 'json', spec['output']]))
        require(int(probed['streams'][0]['nb_read_frames']) == frames, probed)
        command([ORACLE_FFMPEG, '-v', 'error', '-xerror', '-i', spec['output'], '-f', 'null', '-'])
        decoded.append(probed)
    return dict(accepted=accepted, final=state, identities=identities, decoded=decoded, expected_frames=frames)


def frame_rows(prefix):
    prefix = Path(prefix)
    def read():
        paths = sorted(prefix.parent.glob(prefix.name + '.*.jsonl'))
        if not paths or list(prefix.parent.glob(prefix.name + '.*.active')):
            return None
        return [row for path in paths for line in path.read_text().splitlines()
                if (row := json.loads(line)).get('event') == 'frame']
    return wait(read)


def lifecycle(harness, sample, frames):
    base = dict(id='base', output=str(harness.run / 'base.mp4'), output_format='mp4',
                width=160, height=96, encoder_preset='ultrafast', audio_copy=False)
    config = dict(input=str(sample), outputs=[base], rendition_backend='process', idle_timeout_ms=3000,
                  pace=True, max_threads=2, min_threads=2, decoder_auto_threads=False, frame_log=True,
                  timestamps={'mode': 'source_all'})
    harness.api(body=config, expected=202)
    state = wait(lambda: (s if (s := harness.api()).get('packets_out', 0) >= 8 else None))
    job = state['job_id']
    original = state['outputs'][0]
    original_pid = original['rendition_pid']
    events = []

    def operation(body):
        result = harness.api(f'/v1/jobs/{job}/updates', body, 202)
        events.append(dict(request=body, accepted=result))
        return result

    def terminal(body, written=False):
        def check():
            r = harness.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}')
            require(r['state'] not in ('failed', 'cancelled', 'superseded'), r)
            if r['state'] == 'applied' and (not written or r['delivery']['state'] == 'first_written'):
                return r
            return None
        result = wait(check)
        events[-1]['final'] = result
        return result

    def request(action, output, spec=None):
        current = harness.api()
        body = dict(operation_id=uuid.uuid4().hex, action=action,
                    target=dict(component='rendition', output_id=output), expected_revision=current['topology_revision'])
        if spec is not None:
            body['spec'] = spec
        return body

    extra = dict(base, id='extra', output=str(harness.run / 'extra.mp4'))
    add = request('add_rendition', 'extra', extra)
    operation(add)
    attached = terminal(add, written=True)
    replay = harness.api(f'/v1/jobs/{job}/updates', add, (200, 202))
    require(replay['operation_id'] == add['operation_id'], replay)
    state = wait(lambda: (s if len((s := harness.api()).get('outputs', [])) == 2 else None))
    healthy = next(b for b in state['outputs'] if b['output_id'] == 'base')
    new = next(b for b in state['outputs'] if b['output_id'] == 'extra')
    require(healthy['rendition_pid'] == original_pid, 'existing encoder restarted during hot add')
    require(new['rendition_pid'] != original_pid, 'hot add shares encoder process')
    require(new['rendition_generation'] > original['rendition_generation'], 'generation not monotonic')
    def extra_progress():
        current = harness.api()
        return current if next((b['packets_out'] for b in current.get('outputs', []) if b['output_id'] == 'extra'), 0) >= 8 else None
    wait(extra_progress)
    remove = request('remove_rendition', 'extra')
    operation(remove)
    detached = terminal(remove)
    wait(lambda: len(harness.api().get('outputs', [])) == 1)
    expected = list(range(attached['applied_checkpoint']['frame_id'], detached['applied_checkpoint']['frame_id'] + 1))
    rows = frame_rows(new['log_path'])
    require([row['frame_id'] for row in rows] == expected, ('hot interval frame sequence', expected, [r['frame_id'] for r in rows]))
    require(len({row['input_sequence'] for row in rows}) == len(expected), 'duplicate source sequence')
    last = request('remove_rendition', 'base')
    operation(last)
    base_removed = terminal(last)
    idle = wait(lambda: (s if not (s := harness.api()).get('outputs') and s['idle_deadline_us'] > 0 else None))
    require(idle['state'] not in ('failed', 'completed', 'stopped'), idle['state'])
    reborn = dict(base, id='reborn', output=str(harness.run / 'reborn.mp4'))
    add_again = request('add_rendition', 'reborn', reborn)
    operation(add_again)
    resumed = terminal(add_again, written=True)
    state = wait(lambda: (s if (s := harness.api()).get('outputs') and s['idle_deadline_us'] == 0 else None))
    new_pid = state['outputs'][0]['rendition_pid']
    require(new_pid not in (original_pid, new['rendition_pid']), 'new incarnation reuses live identity')
    final_remove = request('remove_rendition', 'reborn')
    operation(final_remove)
    terminal(final_remove)
    final = wait(lambda: (s if (s := harness.api())['state'] in ('completed', 'failed', 'stopped', 'completed_partial') else None), 12)
    require(final['state'] == 'completed' and final['frames_decoded'] < frames, ('idle expiry not effective', final['state'], final['frames_decoded'], frames))
    require(not final['outputs'] and final['shared_pool_occupied_slots'] == 0, ('idle retained media', final))
    base_rows = frame_rows(original['log_path'])
    require([row['frame_id'] for row in base_rows] == list(range(1, base_removed['applied_checkpoint']['frame_id'] + 1)), 'healthy source sequence changed')
    return dict(events=events, original_pid=original_pid, added_pid=new['rendition_pid'], resumed_pid=new_pid,
                final=final, fixed_input_frames=frames, hot_interval_expected=expected, hot_interval_count=len(rows), resumed=resumed)


def decoded_hashes(path):
    text = command([ORACLE_FFMPEG, '-v', 'error', '-xerror', '-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough', '-f', 'framemd5', '-']).decode()
    return [line.rsplit(',', 1)[1].strip() for line in text.splitlines() if line and not line.startswith('#')]


def fault_case(harness, sample, frames, fault):
    outputs = [dict(id=name, output=str(harness.run / (name + '.mp4')), output_format='mp4', width=160, height=96,
                    encoder_preset='ultrafast', audio_copy=False, max_lag_ms=(4000 if fault in ('hold_raw', 'stop_release') else 1000) if name == 'bad' else 10000)
               for name in ['bad', 'good']]
    config = dict(input=str(sample), outputs=outputs, rendition_backend='process', pace=True, max_threads=2,
                  min_threads=2, decoder_auto_threads=False, frame_log=True, timestamps={'mode': 'source_all'})
    harness.api(body=config, expected=202)
    signal_number = {'sigsegv': signal.SIGSEGV, 'sigabrt': signal.SIGABRT, 'sigkill': signal.SIGKILL, 'sigstop': signal.SIGSTOP}.get(fault)
    identities = {}
    healthy_progress = []
    heartbeat_samples = []
    root_pid = None
    injected = False
    def sample_state():
        nonlocal injected, root_pid
        s = harness.api()
        control_pid = s.get('supervisor', {}).get('worker_pid')
        if root_pid is None and control_pid:
            root_pid = control_pid
        if control_pid:
            require(control_pid == root_pid, 'shared input worker replaced')
        by_id = {b['output_id']: b for b in s.get('outputs', [])}
        for name, output in by_id.items():
            if output.get('rendition_pid'):
                before = identities.setdefault(name, (output['rendition_pid'], output['rendition_generation']))
                require(before == (output['rendition_pid'], output['rendition_generation']), 'unexpected automatic recovery in manual mode')
        good = by_id.get('good', {})
        bad = by_id.get('bad', {})
        if good.get('packets_out'):
            healthy_progress.append(good['packets_out'])
        if fault in ('busy', 'deadlock', 'hold_raw') and bad.get('rendition_pid'):
            heartbeat_samples.append((bad.get('packets_out', 0), bad.get('progress', {}).get('sampled_monotonic_us', 0)))
        if signal_number and not injected and bad.get('packets_out', 0) >= 8 and good.get('packets_out', 0) >= 8:
            descriptor = os.pidfd_open(bad['rendition_pid'])
            try:
                signal.pidfd_send_signal(descriptor, signal_number)
            finally:
                os.close(descriptor)
            injected = True
        (harness.run / 'latest-state.json').write_text(json.dumps(s, indent=2) + '\n')
        return s if s['state'] in ('completed', 'completed_partial', 'failed', 'stopped') else None
    final = wait(sample_state, 30)
    require(final['state'] == 'completed_partial', {k: final.get(k) for k in ['state','error','frames_decoded']})
    require(final['frames_decoded'] == frames, 'shared decoder did not complete the fixed input')
    if signal_number:
        require(injected, 'fault was never injected')
    by_id = {b['output_id']: b for b in final['outputs']}
    good, bad = by_id['good'], by_id['bad']
    require(good['state'] == 'completed' and good['packets_out'] == frames, good)
    require(bad['state'] == 'failed' and bad['exit_status_known'], bad)
    expected_signal = signal.SIGSEGV if fault == 'write_shared' else signal.SIGKILL if fault in ('busy','deadlock','hold_raw','sigstop','sigkill') else signal.SIGABRT if fault == 'sigabrt' else signal.SIGSEGV if fault == 'sigsegv' else 0
    require(bad['exit_signal'] == expected_signal, ('wrong exit cause', fault, bad['exit_signal'], bad['exit_code']))
    require(not good['rendition_pid'] and not bad['rendition_pid'], 'holder not reaped')
    require(final['shared_pool_occupied_slots'] == 0, 'shared raw slots did not close')
    require(healthy_progress == sorted(healthy_progress), 'healthy delivery counter regressed')
    rows = frame_rows(good['log_path'])
    require([r['frame_id'] for r in rows] == list(range(1, frames + 1)), 'healthy branch dropped, repeated or reordered source frames')
    hashes = decoded_hashes(outputs[1]['output'])
    require(len(hashes) == frames, ('independent decoder frame count', len(hashes), frames))
    failures = []
    for path in (harness.run / 'logs/events').glob('worker-*.jsonl'):
        failures += [r for line in path.read_text().splitlines() if (r := json.loads(line)).get('event') == 'rendition_failed' and r.get('output_id') == 'bad']
    require(failures, 'failure attribution event missing')
    if fault in ('hold_raw', 'stop_release'):
        require(any(r['retained_raw_leases'] == 32 for r in failures), ('full raw credit case was not reached', failures))
    if fault in ('busy', 'deadlock'):
        observed = {}
        for packets, sampled in heartbeat_samples:
            observed.setdefault(packets, set()).add(sampled)
        require(any(len(values) >= 2 for values in observed.values()), 'did not observe a live control heartbeat while media stalled')
    reports = []
    for path in harness.crash_directory.glob('crash-*.json'):
        report = json.loads(path.read_text())
        if report.get('rendition_id') == 'bad':
            reports.append(report)
    if fault in ('sigsegv', 'sigabrt', 'write_shared'):
        require(any(r.get('signal') == expected_signal and r.get('role') == 'rendition' and r.get('rendition_generation') == bad['rendition_generation'] and r.get('input_session_instance') == final['instance_id'] and r.get('job_id') == final['job_id'] for r in reports), 'generation-aware crash report missing')
    detected=min(row['monotonic_us'] for row in failures)
    def latency(values):
        values=sorted(values)
        return dict(count=len(values),**{name:values[round((len(values)-1)*q)] if values else None for name,q in [('p50_ms',.5),('p95_ms',.95),('p99_ms',.99)]})
    require(all('mux_flush_done_monotonic_us' in row for row in rows),'actual-write clock missing')
    before=latency([row['ingress_to_mux_ms'] for row in rows if row['mux_flush_done_monotonic_us']<detected]);after=latency([row['ingress_to_mux_ms'] for row in rows if row['mux_flush_done_monotonic_us']>=detected])
    require(after['count']>0,'healthy branch did not write after failure detection')
    return dict(fault=fault, final=final, identities=identities, expected_frames=frames, decoded_hashes=hashes,
                failure_detected_us=detected,healthy_latency_before_detection=before,healthy_latency_after_detection=after,
                failure_events=failures, crash_reports=reports, heartbeat_samples=heartbeat_samples)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--phase', choices=['smoke','thread','lifecycle','faults'], default='smoke')
    args = parser.parse_args()
    (ROOT / 'out').mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='rendition-process-', dir=ROOT / 'out'))
    sample = run / 'source.ts'
    frames = 300 if args.phase == 'lifecycle' else 150 if args.phase == 'faults' else 75
    command([ORACLE_FFMPEG, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=25',
             '-frames:v', frames, '-c:v', 'libx264', '-preset', 'ultrafast', '-threads', '2', '-g', '25',
             '-bf', '3', '-f', 'mpegts', sample])
    harness = Harness(run)
    report = dict(run=str(run), mode='local-emulated-linux-functional', capacity_claim=False)
    try:
        report['health'] = harness.start()
        if args.phase == 'faults':
            harness.close()
            report['faults'] = []
            baseline_dir = run / 'baseline'
            baseline_dir.mkdir()
            baseline = Harness(baseline_dir, BIN / 'rendition-faults')
            try:
                baseline.start()
                report['baseline'] = smoke(baseline, sample, frames)
                reference_hashes = decoded_hashes(baseline_dir / 'out1.mp4')
                require(len(reference_hashes) == frames, 'baseline independent frame count')
            finally:
                baseline.close()
            for fault in ['sigsegv','sigabrt','sigkill','sigstop','busy','deadlock','hold_raw','stop_release','write_shared']:
                directory = run / fault
                directory.mkdir()
                case = Harness(directory, BIN / 'rendition-faults', {'NAKI_TEST_RENDITION_FAULT': fault})
                try:
                    case.start()
                    result = fault_case(case, sample, frames, fault)
                    require(result['decoded_hashes'] == reference_hashes, ('healthy decoded pixels differ from baseline', fault))
                    report['faults'].append(result)
                    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
                finally:
                    case.close()
        else:
            report[args.phase] = lifecycle(harness, sample, frames) if args.phase == 'lifecycle' else smoke(harness, sample, frames, 'thread' if args.phase == 'thread' else 'process', 4)
        report['passed'] = True
    finally:
        harness.close()
        (run / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'run': str(run), 'passed': report.get('passed', False)}))


if __name__ == '__main__':
    main()
