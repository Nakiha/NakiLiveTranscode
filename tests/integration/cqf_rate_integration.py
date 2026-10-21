#!/usr/bin/env python3
"""Real CQF, integer decimation, protected source I and independent VVC/PTS checks."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import pathlib
import signal
import subprocess
import tempfile
import time
from support.service_harness import ServiceHarness, new_run, PROBE
from support.vvc_structure import decode_frames
from integration.frame_policy_integration import check_vvc_intra_groups, refresh_packets


def run():
    directory = new_run('cqf-rate-')
    client = ServiceHarness()
    source = directory / 'source.mp4'
    command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=60',
               '-frames:v', '480', '-vf', 'setpts=PTS+2/TB', '-fps_mode', 'passthrough',
               '-c:v', 'libx264', '-preset', 'veryfast', '-threads', '2', '-g', '120', '-bf', '3',
               '-x264-params', 'scenecut=0:open-gop=0', str(source)]
    subprocess.run(command, check=True, timeout=45)
    reference = decode_frames(PROBE, source)
    assert len(reference) == 480
    assert [i for i, f in enumerate(reference) if f['pict_type'] == 'I'] == [0, 120, 240, 360]
    results = []
    with tempfile.TemporaryDirectory(prefix='cqf-rate-state-') as state_dir, \
         (directory / 'controller.log').open('w') as log:
        server = subprocess.Popen([str(client.BIN / 'naki-live'), '--port', str(client.PORT),
                                   '--state-dir', state_dir, '--run-dir', str(directory / 'logs')], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    if client.http('/health')['admission']['open']:
                        break
                except OSError:
                    pass
                assert time.monotonic() < deadline
                time.sleep(.03)
            for qp in (28, 40, 32):
                output = directory / f'qp-{qp}.flv'
                config = dict(input=str(source), output=str(output), output_format='flv', encoder='libvvenc',
                              rate_control='cqf', qp=qp, maxrate=6000000, output_fps=30, width=256, height=144,
                              encoder_preset='faster', encoder_threads=2, frame_log=True,
                              timestamps=dict(mode='source_all'), intra=dict(mode='follow_source'))
                if qp==32:config['encoder_max_future_frames']=4
                client.start(config)
                state = client.finished(timeout=60)
                assert state['frames_decoded'] == 480 and state['frames_dropped_rate'] == 240, state
                assert state['packets_out'] == state['frames_filtered'] == state['frames_selected'] == 240
                assert state['intra_requested'] == state['intra_confirmed'] == 4
                assert state['rate_control'] == 'cqf' and state['qp'] == qp and state['qpa'] == 1
                assert state['video_rate']['num'] == 60 and state['video_rate']['decimation_divisor'] == 2
                assert state['video_rate']['rc_nominal_rate'] == dict(num=30, den=1)
                decoded = decode_frames(PROBE, output)
                assert len(decoded) == 240
                actual = [float(f['pts_time']) for f in decoded]
                expected = [float(f['pts_time']) for f in reference[::2]]
                assert all(abs(a-b) < .0011 for a,b in zip(actual,expected)), (actual[:5],expected[:5])
                assert [i for i,f in enumerate(decoded) if f['pict_type'] == 'I'] == [0,60,120,180]
                check_vvc_intra_groups(decoded)
                assert refresh_packets(output, 'libvvenc') == [0]
                prefix = pathlib.Path(state['log_path'])
                deadline = time.monotonic() + 5
                while list(prefix.parent.glob(prefix.name + '.*.active')) and time.monotonic() < deadline:
                    time.sleep(.03)
                trace = [json.loads(line) for p in sorted(prefix.parent.glob(prefix.name + '.*.jsonl')) for line in p.read_text().splitlines()]
                frames = [f for f in trace if f['event'] == 'frame']
                drops = [f for f in trace if f['event'] == 'frame_drop']
                assert len(frames) == len(drops) == 240
                assert all(f['frame_id'] % 2 == 1 for f in frames)
                assert all(f['reason'] == 'output_rate' for f in drops)
                assert state['ingress_to_mux']['count'] == len(frames)
                assert abs(state['ingress_to_mux']['mean_ms'] - sum(f['ingress_to_mux_ms'] for f in frames)/len(frames)) < 1e-6
                assert not state['logging']['dropped_frames']
                if qp==32:
                    assert max(f['encoder_future_frames'] for f in frames)<=4
                    assert state['network_read']['encoder_future_max']<=4
                (directory / f'qp-{qp}-state.json').write_text(json.dumps(state, indent=2))
                results.append(dict(qp=qp, bytes=output.stat().st_size, frames=len(decoded)))
            assert results[0]['bytes'] > results[1]['bytes'], results
            # Picture-scoped QP deltas survive B reorder and EOF; a high cap
            # isolates the quality anchor response from the rate budget.
            controlled=[]
            for quality in (28,40,32,33):
                rc={k:v for k,v in config.items() if k!='qp'}
                rc.update(rate_control='constrained_crf',crf=quality,maxrate=100000000,
                          decoder_pump=True,reader_queue_capacity=1,queue_capacity=1,min_threads=1,budget_slots=1,max_threads=2,
                          output=str(directory/f'crf-{quality}.flv'))
                if quality==32:rc["encoder_profile"]="live_3b_2s_speed"
                if quality==33:rc["encoder_profile"]="live_3b_2s_rdoq0"
                client.start(rc); state=client.finished(timeout=60)
                assert state['rate_control']=='constrained_crf' and state['crf']==quality
                assert state['frames_decoded']==480 and state['frames_dropped_rate']==240
                assert state['packets_out']==240 and state['network_read']['encoder_future_max']<=4
                pictures=decode_frames(PROBE,rc['output']);check_vvc_intra_groups(pictures)
                assert refresh_packets(rc['output'],'libvvenc')==[0]
                assert [i for i,f in enumerate(pictures) if f['pict_type']=='I']==[0,60,120,180]
                prefix=pathlib.Path(state['log_path'])
                deadline=time.monotonic()+5
                while list(prefix.parent.glob(prefix.name+'.*.active')) and time.monotonic()<deadline:time.sleep(.03)
                events=[json.loads(line) for p in prefix.parent.glob(prefix.name+'.*.jsonl') for line in p.read_text().splitlines()]
                frames=[e for e in events if e['event']=='frame']
                assert len(frames)==240 and all(0<=e['rc_qp']<=63 and -12<=e['rc_actual_slice_qp']<=63 for e in frames)
                assert len({e['rc_qp'] for e in frames})>1
                controlled.append(pathlib.Path(rc['output']).stat().st_size)
                (directory/f'crf-{quality}-state.json').write_text(json.dumps(state,indent=2))
            assert controlled[0]>controlled[1],controlled
            # Off-grid I pictures must survive decimation, with original PTS.
            odd_source = directory / 'off-grid-i.mp4'
            off_command = command.copy()
            off_command[off_command.index('480')] = '124'
            off_command[off_command.index('120')] = '61'
            off_command[-1] = str(odd_source)
            subprocess.run(off_command, check=True, timeout=30)
            config.update(input=str(odd_source), output=str(directory / 'off-grid-i.flv'))
            client.start(config)
            state = client.finished(timeout=35)
            (directory / 'off-grid-state.json').write_text(json.dumps(state,indent=2))
            off = decode_frames(PROBE, config['output'])
            ref = decode_frames(PROBE, odd_source)
            expected = [f for i,f in enumerate(ref) if i % 2 == 0 or f['pict_type'] == 'I']
            assert len(off) == len(expected) == state['packets_out'] == 63
            assert state['frames_dropped_rate'] == 61 and state['intra_confirmed'] == 3
            assert all(abs(float(a['pts_time'])-float(b['pts_time'])) < .0011 for a,b in zip(off,expected))
            check_vvc_intra_groups(off)
            # Fixed branches inherit CQF, but own their decimation and encoder rate.
            multi = {key:value for key,value in config.items() if key not in ('output','output_fps','width','height','encoder_max_future_frames')}
            # Functional inheritance/cadence check, not an unpaced branch
            # capacity claim. Keep the bounded queue protection active on CI.
            multi.update(input=str(source), frame_log=False, pace=True, outputs=[
                dict(id='half', output=str(directory/'half.flv'), output_fps=30, width=256, height=144),
                dict(id='full', output=str(directory/'full.flv'), output_fps=60, width=160, height=96)])
            # Preserve the original QP40 inheritance workload. The added QP32
            # future-bound case must not silently raise this capacity demand.
            multi['qp']=40
            client.start(multi)
            multi_state = client.finished(timeout=60)
            (directory / 'branches-state.json').write_text(json.dumps(multi_state,indent=2))
            branches = multi_state['outputs']
            for branch,step in zip(branches,(2,1)):
                assert branch['frames_decoded'] == 480 and branch['frames_dropped_rate'] == 480-480//step, branch
                assert branch['packets_out'] == 480//step
                decoded = decode_frames(PROBE,directory/(branch['output_id']+'.flv'))
                assert len(decoded) == 480//step
                assert all(abs(float(a['pts_time'])-float(b['pts_time'])) < .0011 for a,b in zip(decoded,reference[::step]))
                check_vvc_intra_groups(decoded)
            # A noninteger ratio fails explicitly, rather than silently changing cadence.
            config.update(input=str(source), output=str(directory / 'invalid-ratio.flv'), output_fps=24)
            client.start(config)
            rejected = client.finished('failed', timeout=10)
            (directory / 'invalid-ratio-state.json').write_text(json.dumps(rejected,indent=2))
            assert 'integer input/output ratio' in rejected['error'], rejected
            config.update(output=str(directory/'future-bound-rejected.flv'),output_fps=30,encoder_max_future_frames=1)
            client.start(config)
            rejected=client.finished('failed',timeout=20)
            (directory/'future-bound-rejected.json').write_text(json.dumps(rejected,indent=2))
            assert 'buffer space' in rejected['error'].lower(),rejected
        finally:
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=20)
    assert server.returncode == 0
    (directory / 'COMPLETE.json').write_text(json.dumps(dict(status='passed', results=results), indent=2))
    print('PASS CQF QP response, 60->30 PTS/I/3B/IDR, protected off-grid I, trace accounting and invalid ratio:', directory, flush=True)


if __name__ == '__main__':
    run()
