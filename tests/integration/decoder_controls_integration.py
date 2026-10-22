#!/usr/bin/env python3
"""Functional full-service backend x early-output check; not a capacity benchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from support.service_harness import ServiceHarness, port


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class ControlsHarness(ServiceHarness):
    def __init__(self, directory):
        super().__init__()
        self.directory = directory

    @property
    def RUN(self):
        return self.directory


def traces(state, expected):
    prefix = Path(state['log_path'])
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        files = sorted(prefix.parent.glob(prefix.name + '.*.jsonl'))
        frames = [json.loads(line) for file in files for line in file.read_text().splitlines() if line.strip()]
        frames = [f for f in frames if f.get('event') == 'frame']
        if len(frames) == expected and not list(prefix.parent.glob(prefix.name + '.*.active')):
            return frames
        time.sleep(.02)
    raise AssertionError(f'expected {expected} sealed frame traces, got {len(frames)}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--input', type=Path)
    ap.add_argument('--expected-frames', type=int, default=48)
    ap.add_argument('--results', type=Path)
    args = ap.parse_args()
    if args.expected_frames <= 0:
        raise ValueError('expected-frames must be declared positive')
    if args.results:
        directory = args.results.resolve(); directory.mkdir(parents=True, exist_ok=False)
    else:
        import tempfile
        (ROOT / 'out').mkdir(exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix='decoder-controls-', dir=ROOT / 'out'))
    source = args.input.resolve() if args.input else directory / 'hrd-b3.mp4'
    if not args.input:
        command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=60', '-frames:v', str(args.expected_frames),
                   '-c:v', 'libx264', '-threads', '2', '-preset', 'veryfast', '-x264-params',
                   'keyint=48:min-keyint=48:scenecut=0:bframes=3:b-adapt=0:force-cfr=1:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=2400', '-an', str(source)]
        (directory / 'generate-command.json').write_text(json.dumps(command) + '\n')
        subprocess.run(command, check=True, timeout=45)
    identity = hashlib.sha256(source.read_bytes()).hexdigest()
    harness = ControlsHarness(directory)
    with (directory / 'backend-probe.log').open('x') as probe_log:
        subprocess.run([str(harness.BIN / 'naki-decoder-backend-probe'), str(source), str(args.expected_frames)],
                       stdout=probe_log, stderr=subprocess.STDOUT, check=True, timeout=60)
    log = (directory / 'controller.jsonl').open('x')
    stderr = (directory / 'controller.stderr').open('x')
    server = subprocess.Popen([str(harness.BIN / 'naki-live'), '--port', str(harness.PORT), '--run-dir', str(directory / 'logs')],
                              cwd=ROOT, stdout=log, stderr=stderr)
    results, reference = [], None
    try:
        for _ in range(200):
            if server.poll() is not None: raise RuntimeError('controller exited before health')
            try:
                harness.http('/health'); break
            except OSError:
                time.sleep(.03)
        else: raise TimeoutError('controller health')
        for asynchronous in [False, True]:
            for early in [False, True]:
                label = f'async{int(asynchronous)}-early{int(early)}'
                config = {'input': str(source), 'output': str(directory / (label + '.ts')), 'width': 320, 'height': 180,
                          'encoder_threads': 1, 'max_threads': 2, 'min_threads': 2, 'queue_capacity': 1,
                          'decoder_async': asynchronous, 'decoder_auto_threads': False, 'decoder_early_output': early,
                          'audio_copy': False, 'frame_log': True, 'frame_log_every': 1, 'pace': False}
                (directory / (label + '-request.json')).write_text(json.dumps(config, indent=2) + '\n')
                harness.start(config)
                state = harness.finished(timeout=60)
                (directory / (label + '-state.json')).write_text(json.dumps(state, indent=2) + '\n')
                require(state['decoder_async'] == int(asynchronous) and state['decoder_auto_threads'] == 0, 'wrong decoder backend status')
                require(state['decoder_early_output_requested'] == int(early), 'wrong early-output request status')
                require(state['frames_decoded'] == state['frames_filtered'] == state['packets_out'] == args.expected_frames, 'wrong complete service frame count')
                frames = traces(state, args.expected_frames)
                source_ids = [x['input_sequence'] for x in frames]
                require(len(set(source_ids)) == args.expected_frames, 'duplicate/missing source identities')
                ordered = [(x['pts_us'], x['input_sequence'], x['decoded_picture_type'], x['decoded_key']) for x in frames]
                require([x[0] for x in ordered] == sorted(x[0] for x in ordered), 'presentation order changed')
                if reference is None: reference = ordered
                else: require(ordered == reference, 'cross-backend source identity/PTS mismatch')
                for f in frames:
                    require(f['decoder_async'] == int(asynchronous), 'wrong frame backend')
                    require(f['decoder_auto_threads'] == 0, 'auto controller unexpectedly active')
                    for field in ['decoder_submit_retry_ms', 'decoder_submit_to_receive_ms', 'decoder_receive_to_prepare_ms']:
                        require(f[field] >= 0, 'negative ' + field)
                # Decode the actual output rather than trusting packet counts.
                command = ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames', '-show_entries', 'stream=nb_read_frames', '-of', 'json', config['output']]
                decoded = json.loads(subprocess.check_output(command, text=True))['streams']
                require(len(decoded) == 1 and int(decoded[0]['nb_read_frames']) == args.expected_frames, 'output media has wrong decoded count')
                results.append({'case': label, 'passed': True, 'frames': args.expected_frames})
                (directory / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                print(label, 'PASS', flush=True)
        require(hashlib.sha256(source.read_bytes()).hexdigest() == identity, 'input changed during service test')
        (directory / 'COMPLETE.json').write_text(json.dumps({'status': 'functional-four-combinations-passed', 'input_sha256': identity,
            'expected_frames': args.expected_frames, 'performance_acceptance': False}) + '\n')
    finally:
        server.terminate()
        try: server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server.kill(); server.wait(); raise
        log.close(); stderr.close()
    print(directory)


if __name__ == '__main__':
    main()
