#!/usr/bin/env python3
"""Compare legacy/one/two/four output lifecycles with bounded owner fault gates."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import threading
import time

from support import control_server as control

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = pathlib.Path(os.environ.get('NAKI_SERVICE_BIN', ROOT / 'out/bin'))


class Sink:
    """Real TCP protocol close, retaining the bytes for independent decoding."""
    def __init__(self, path):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(.1)
        self.url = f'tcp://127.0.0.1:{self.listener.getsockname()[1]}'
        self.stop = threading.Event()
        self.done = threading.Event()
        self.error = None

        def receive():
            try:
                while not self.stop.is_set():
                    try:
                        connection, _ = self.listener.accept()
                        break
                    except socket.timeout:
                        continue
                else:
                    return
                with connection, path.open('wb') as stream:
                    connection.settimeout(.1)
                    while not self.stop.is_set():
                        try:
                            data = connection.recv(65536)
                            if not data:
                                break
                            stream.write(data)
                        except socket.timeout:
                            continue
            except OSError as error:
                if not self.stop.is_set():
                    self.error = repr(error)
            finally:
                self.done.set()

        self.thread = threading.Thread(target=receive)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive() and not self.error, self.error


def command(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=30)
    assert result.returncode == 0, (args, result.stderr[-4000:])
    return result.stdout


def audio_payloads(path):
    data = command(['ffmpeg', '-v', 'error', '-i', path, '-map', '0:a:0', '-c:a', 'copy', '-f', 'adts', '-'])
    items, pos = [], 0
    while pos < len(data):
        assert data[pos] == 255 and data[pos + 1] & 0xf6 == 0xf0
        size = ((data[pos + 3] & 3) << 11) | (data[pos + 4] << 3) | (data[pos + 5] >> 5)
        header = 7 if data[pos + 1] & 1 else 9
        assert size >= header and pos + size <= len(data)
        items.append(hashlib.sha256(data[pos + header:pos + size]).hexdigest())
        pos += size
    return items


def inspect_output(path, packets, source_audio):
    data = json.loads(command(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
                              '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'json', path]))
    frames = data['frames']
    assert len(frames) == packets, (path, len(frames), packets)
    pts = [round(float(f['best_effort_timestamp_time']) * 1000000) for f in frames]
    assert all(b > a for a, b in zip(pts, pts[1:])), pts
    encoded = json.loads(command(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_packets',
                                 '-show_entries', 'packet=pts_time,dts_time', '-of', 'json', path]))['packets']
    dts = [float(p['dts_time']) for p in encoded]
    assert all(b > a for a, b in zip(dts, dts[1:])), dts
    assert all(float(p['dts_time']) <= float(p['pts_time']) for p in encoded)
    audio = audio_payloads(path)
    assert audio
    offset = 0
    for payload in audio:
        offset = source_audio.index(payload, offset) + 1
    return dict(frames=len(frames), pts_us=pts, audio_payloads=audio)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=pathlib.Path, default=BIN / 'naki-live-hook-worker')
    parser.add_argument('--modes', help='Comma-separated subset for focused debugging')
    parser.add_argument('--pace', action='store_true', help='Offer input at its media rate; production queue limits are unchanged')
    args = parser.parse_args()
    run = pathlib.Path(tempfile.mkdtemp(prefix='pipeline-lifecycle-', dir=ROOT / 'out'))
    binaries = run / 'bin'
    binaries.mkdir()
    for name in ('naki-live', 'naki-crash-collector'):
        shutil.copy2(BIN / name, binaries / name)
    shutil.copy2(args.worker, binaries / 'naki-live-worker')
    sample = run / 'input.ts'
    command(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=25:duration=0.96',
             '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=0.96',
             '-c:v', 'libx264', '-preset', 'ultrafast', '-threads', '2', '-bf', '3', '-g', '25',
             '-c:a', 'aac', '-b:a', '64k', '-f', 'mpegts', sample])
    empty = run / 'empty.ts'
    empty.touch()
    source_audio = audio_payloads(sample)
    modes = ['normal', 'empty_input', 'empty', 'decoder', 'async', 'filter', 'encoder', 'output', 'open',
             'custom_io', 'header', 'reader_thread', 'decode', 'video', 'all_video', 'primary_trailer',
             'trailer', 'flush', 'close', 'stop', 'stop_close', 'queue', 'branch_thread']
    if args.modes:
        selected = set(args.modes.split(','))
        assert selected <= set(modes)
        modes = [m for m in modes if m in selected]
    results = {}
    for layout, n in (('legacy', 0), ('one', 1), ('two', 2), ('four', 4)):
        for mode in modes:
            if n < 2 and mode in ('queue', 'branch_thread', 'all_video'):
                continue
            name = layout + '-' + mode
            case = run / name
            case.mkdir()
            stopping = mode in ('stop', 'stop_close')
            injection = {'empty_input': 'normal', 'all_video': 'video', 'stop': 'normal', 'stop_close': 'close'}.get(mode, mode)
            shared = mode in ('empty', 'decoder', 'async', 'reader_thread', 'decode', 'queue', 'branch_thread')
            target = 'shared' if shared and n > 1 else 'main' if not n else 'out0'
            if mode == 'all_video':
                target = 'all'
            environment = dict(NAKI_TEST_MEDIA_HOOK_DIR=str(case), NAKI_TEST_HOOK_OUTPUT_PATHS='1',
                               NAKI_TEST_LIFECYCLE_DIR=str(case), NAKI_TEST_LIFECYCLE_MODE=injection,
                               NAKI_TEST_LIFECYCLE_TARGET=target)
            server = control.Server(run, name, binaries / 'naki-live', environment=environment)
            accepted = None
            sinks = []
            try:
                outputs = []
                for i in range(n or 1):
                    path = case / f'out{i}.ts'
                    if mode != 'custom_io':
                        sink = Sink(path)
                        sinks.append(sink)
                    outputs.append(dict(id=f'out{i}', output=str(path) if mode == 'custom_io' else sink.url,
                                        output_format='mpegts', encoder_preset='ultrafast', encoder_threads=2))
                spec = dict(input=str(empty if mode == 'empty_input' else sample), pace=stopping or args.pace,
                            timestamps={'mode': 'source_all'}, frame_log=True, frame_log_every=1)
                if n:
                    spec['outputs'] = outputs
                else:
                    spec.update({k: v for k, v in outputs[0].items() if k != 'id'})
                accepted = server.api(body=spec, code=202)
                if stopping:
                    control.wait(lambda: server.api()['packets_out'] >= 4)
                    server.api('/v1/job/stop', dict(job_id=accepted['job_id']), 202)
                state = control.wait(lambda: (v if (v := server.api())['state'] in
                                             ('completed', 'completed_partial', 'failed', 'stopped') else None), 15)
                audit = control.wait(lambda: json.loads((case / 'lifecycle-audit.json').read_text())
                                     if (case / 'lifecycle-audit.json').exists() else None)
                private = mode in ('filter', 'encoder', 'output', 'open', 'custom_io', 'header', 'video',
                                   'primary_trailer', 'trailer', 'flush', 'close', 'branch_thread')
                expected = 'stopped' if stopping else 'completed' if mode == 'normal' else 'completed_partial' if private and n > 1 else 'failed'
                assert state['state'] == expected, (name, state)
                rows = state.get('outputs', [state])
                semantic = []
                for i, row in enumerate(rows):
                    identity = row.get('output_id', 'main' if not n else f'out{i}')
                    final = row['output_finalize']
                    if n > 1:
                        assert not any(row[k] for k in ('queue_items', 'queue_bytes', 'inflight_frames', 'inflight_bytes')), row
                        assert row['queue_high_items'] <= row['queue_limits']['total_items']
                        assert row['queue_video_high'] <= row['queue_limits']['video_items']
                        assert row['queue_audio_high'] <= row['queue_limits']['audio_packets']
                        assert row['inflight_high_frames'] <= 128
                    assert not row['watermark']['active_frames'] and not row['watermark']['active_raw_bytes'], row
                    assert not row['components']['watermark']['invariant_error'], row
                    hook_file = case / f"{accepted['job_id']}-{identity}.json"
                    hook = json.loads(hook_file.read_text()) if hook_file.exists() else None
                    if hook:
                        # With all branches failing, the shared owner may set
                        # cancel while an already-failed branch is finalizing.
                        if mode != 'all_video':
                            assert hook['cancelled'] == int(stopping or (shared and n > 1 and mode not in ('empty', 'branch_thread'))), (name, row, hook)
                        assert hook['error_code'] == final['primary_error'], (name, row, hook)
                    hit = identity == target or target == 'all'
                    if hit and mode in ('trailer', 'primary_trailer'):
                        assert final['trailer_error'] == -5, final
                        assert final['primary_error'] == (-542398533 if mode == 'primary_trailer' else -5), final
                    if hit and mode in ('close', 'stop_close', 'flush'):
                        assert final['close_error' if mode != 'flush' else 'flush_error'] == -28, final
                        if not stopping:
                            assert final['primary_error'] == -28, final
                        else:
                            assert final['primary_error'] != -28, final
                    media = None
                    if row['state'] == 'completed' or (n < 2 and mode == 'normal'):
                        assert row['packets_out'] == 24, row
                        if sinks:
                            assert sinks[i].done.wait(2) and not sinks[i].error, sinks[i].error
                        media = inspect_output(case / f'out{i}.ts', 24, source_audio)
                    semantic.append(dict(id=identity, state=row['state'],
                                         finalize={k: final[k] for k in ('trailer_error', 'flush_error', 'close_error')},
                                         primary_error=final['primary_error'] if row['state'] != 'stopped' and mode != 'decode' else None,
                                         hook_error=hook['error_code'] if hook and row['state'] != 'stopped' and mode != 'decode' else None,
                                         media=media))
                if injection != 'normal':
                    assert sum(o['faults'] for o in audit['owners']) > 0, (name, audit)
                assert all(o['balanced'] and o['hooks'] == 1 for o in audit['owners'])
                for pid in (state['supervisor']['worker_pid'],):
                    # Acknowledgement may already have reaped the worker. If it
                    # is still alive, none of the media files may remain open.
                    try:
                        for fd in pathlib.Path(f'/proc/{pid}/fd').iterdir():
                            try:
                                target_path = os.readlink(fd)
                            except FileNotFoundError:
                                continue
                            assert target_path not in {str(sample), *(str(case / f'out{i}.ts') for i in range(n or 1))}, target_path
                    except FileNotFoundError:
                        pass
                (case / 'terminal.json').write_text(json.dumps(state, indent=2) + '\n')
                root_primary = state['output_finalize']['primary_error']
                root_expected = {'decoder': -12, 'async': -12, 'decode': -22, 'reader_thread': -11, 'queue': -12}
                if mode in root_expected:
                    assert root_primary == root_expected[mode], (name, state)
                results[name] = dict(state=state['state'], primary_error=None if stopping else root_primary, outputs=semantic)
                print('PASS lifecycle', name, flush=True)
            finally:
                server.close()
                for sink in sinks:
                    sink.close()
            assert server.process.returncode == 0, (name, server.process.returncode)
            control.wait(lambda: control.dead(audit['pid']))
    (run / 'COMPLETE.json').write_text(json.dumps(dict(status='passed', cases=len(results), paced_input=args.pace,
        input_sha256=hashlib.sha256(sample.read_bytes()).hexdigest(), results=results), indent=2) + '\n')
    print('PASS lifecycle matrix:', run, flush=True)


if __name__ == '__main__':
    main()
