#!/usr/bin/env python3
"""HEVC DPB/async and VVenC timeline correlation, EOF and opt-in equivalence."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import importlib.util
import json
import os
import pathlib
import signal
import subprocess
import tempfile
import time
from support.service_harness import ServiceHarness, new_run, PROBE, ROOT
from support.vvc_structure import decode_frames
from integration.frame_policy_integration import check_vvc_intra_groups, refresh_packets


def run():
    directory = new_run('codec-phases-')
    client = ServiceHarness()
    source = directory/'hevc.mp4'
    cmd = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=60',
           '-frames:v', '240', '-vf', 'setpts=PTS+2/TB', '-fps_mode', 'passthrough',
           '-c:v', 'libx265', '-preset', 'veryfast', '-b:v', '1000k',
           '-x265-params', 'pools=2:frame-threads=2:keyint=120:min-keyint=120:scenecut=0:open-gop=0:bframes=3:b-adapt=0', str(source)]
    subprocess.run(cmd, check=True, timeout=45, capture_output=True)
    reference = decode_frames(PROBE, source)
    assert len(reference) == 240
    spec = importlib.util.spec_from_file_location('codec_audit', ROOT/'scripts/audit-codec-phases.py')
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    packets = []
    results = []
    with tempfile.TemporaryDirectory(prefix='codec-phases-state-') as state_dir, (directory/'controller.log').open('w') as log:
        server = subprocess.Popen([str(client.BIN/'naki-live'), '--port', str(client.PORT),
                                   '--state-dir', state_dir, '--run-dir', str(directory/'logs')], stdout=log, stderr=log,
                                  env=dict(os.environ, VVENC_LIVE_CPU_LEDGER='1'))
        try:
            until = time.monotonic()+10
            while True:
                try:
                    if client.http('/health')['admission']['open']:
                        break
                except OSError:
                    pass
                assert time.monotonic() < until
                time.sleep(.03)
            for depth, input_depth, native_ref, enabled in [(10,10,False,False),(10,10,False,True),(8,10,False,False),(8,10,False,True),(8,8,False,False),(8,8,False,True),(8,8,True,False),(8,8,True,True)]:
                case = directory/f'{depth}-{input_depth}-{int(native_ref)}-{int(enabled)}';case.mkdir()
                output = case/'output.flv'
                config = dict(input=str(source), output=str(output), output_format='flv', encoder='libvvenc',
                              rate_control='constrained_crf', crf=32, maxrate=100000000,
                              output_fps=30, width=256, height=144, encoder_preset='faster',
                              encoder_profile='live_3b_2s_speed', encoder_threads=8,
                              encoder_max_future_frames=4, decoder_pump=True, codec_trace=enabled,
                              max_threads=2, min_threads=2, budget_slots=2, queue_capacity=1,
                              reader_queue_capacity=1, frame_log=True,
                              timestamps=dict(mode='source_all'), intra=dict(mode='follow_source'))
                config['encoder_bit_depth'] = depth
                config['encoder_input_bit_depth'] = input_depth
                config['encoder_native8_reference'] = native_ref
                client.start(config);state = client.finished(timeout=60)
                assert state['frames_decoded'] == 240 and state['packets_out'] == 120, state
                assert state['frames_dropped_rate'] == 120 and state['frames_dropped_overload'] == 0
                assert state['codec_trace'] == enabled
                assert state['encoder_bit_depth'] == depth
                assert state['encoder_input_bit_depth'] == input_depth
                assert state['encoder_native8_reference'] == native_ref
                probe = json.loads(subprocess.check_output([str(PROBE), '-v', 'error', '-select_streams', 'v:0',
                    '-show_entries', 'stream=pix_fmt', '-of', 'json', str(output)], text=True))
                assert probe['streams'][0]['pix_fmt'] == ('yuv420p' if depth == 8 else 'yuv420p10le')
                frames = decode_frames(PROBE, output)
                assert len(frames) == 120
                assert all(abs(float(a['pts_time'])-float(b['pts_time'])) < .0011 for a, b in zip(frames, reference[::2]))
                check_vvc_intra_groups(frames)
                assert refresh_packets(output, 'libvvenc') == [0]
                prefix = pathlib.Path(state['log_path'])
                until = time.monotonic()+5
                while list(prefix.parent.glob(prefix.name+'.*.active')) and time.monotonic() < until:
                    time.sleep(.02)
                events = [json.loads(l) for p in sorted(prefix.parent.glob(prefix.name+'.*.jsonl')) for l in p.read_text().splitlines()]
                if enabled:
                    (case/'final-state.json').write_text(json.dumps(state))
                    result = module.audit(case)
                    assert result['decoded_trace_frames'] == 240 and result['written_frames'] == 120
                    if native_ref:
                        assert sum(f['encoder_internal']['byte_me_blocks'] for f in events if f['event']=='frame') > 0
                    (case/'phase-audit.json').write_text(json.dumps(result, indent=2))
                    ledger = [f['encoder_internal'] for f in events if f['event']=='frame' and f['encoder_internal']['pool_ledger_valid']]
                    assert len(ledger)>=110 and all(e['pool_ledger_enabled']==1 for e in ledger)
                    assert len({e['ledger_caller_tid'] for e in ledger})==1 and ledger[0]['ledger_caller_tid']>0
                    for e in ledger:
                        assert e['pool_tasks_cpu_us']==sum(e[k] for k in ('pool_mctf_me_cpu_us','pool_mctf_filter_cpu_us','pool_ctu_cpu_us','pool_other_cpu_us'))
                        assert e['pool_total_cpu_us']>=sum(e[k] for k in ('pool_scan_cpu_us','pool_yield_cpu_us','pool_lock_cpu_us','pool_tasks_cpu_us'))
                    assert ledger[-1]['pool_mctf_me_cpu_us']>0 and ledger[-1]['pool_mctf_filter_cpu_us']>0
                    assert ledger[-1]['pool_ctu_cpu_us']>0 and ledger[-1]['pool_scan_calls']>0
                    assert all(a['pool_total_cpu_us']<=b['pool_total_cpu_us'] for a,b in zip(ledger,ledger[1:]))
                    assert all(f['encoder_internal']['group_last_input_us'] == f['encoder_internal']['input_us']
                               for f in events if f['event'] == 'frame' and f['output_picture_type'] == 'I')
                else:
                    assert not any(e['event'] == 'decoder_frame' or 'encoder_internal' in e for e in events)
                p = subprocess.run([str(PROBE), '-v', 'error', '-select_streams', 'v:0', '-show_packets',
                                    '-show_entries', 'packet=pts,dts,data_hash', '-show_data_hash', 'sha256',
                                    '-of', 'json', str(output)], capture_output=True, text=True, check=True)
                packets.append(json.loads(p.stdout)['packets'])
                results.append(dict(depth=depth, input_depth=input_depth, native_ref=native_ref, enabled=enabled, frames=120))
            assert all(packets[i] == packets[i+1] for i in (0, 2, 4, 6)), 'Diagnostic telemetry changed coded payload or timestamps'
            assert packets[4] == packets[6], 'Byte reference/SAD changed coded payload or timestamps'
            unsupported = directory/'h264.mp4'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=60',
                            '-frames:v', '4', '-c:v', 'libx264', '-threads', '1', str(unsupported)], check=True)
            config.update(input=str(unsupported), output=str(directory/'rejected.flv'))
            client.start(config)
            rejected = client.finished('failed', timeout=10)
            assert 'codec_trace requires native software HEVC input' in rejected['error'], rejected
        finally:
            server.send_signal(signal.SIGTERM);server.wait(timeout=20)
    assert server.returncode == 0
    (directory/'COMPLETE.json').write_text(json.dumps(dict(status='passed', cases=results,
                                                         payload_equivalence=True), indent=2))
    print('PASS codec phase correlation, HEVC DPB/EOF, I/3B, and on/off payload equivalence:', directory, flush=True)


if __name__ == '__main__':
    run()
