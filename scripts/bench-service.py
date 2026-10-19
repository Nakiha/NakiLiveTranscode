#!/usr/bin/env python3
"""Independent measurements using the test runner's identity and evidence gates."""
import argparse
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('service_runner', ROOT / 'scripts/test-service.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=('latency', 'service', 'rtmp'))
    parser.add_argument('--repeats', type=int, choices=range(1, 11), default=3)
    parser.add_argument('--config', type=pathlib.Path, help='Service JSON with a local input; outputs are isolated per run (RTMP capture for rtmp)')
    parser.add_argument('--seconds', type=float, default=60, help='Maximum observation time per service run; EOF can end it sooner')
    parser.add_argument('--mode', choices=('paced', 'throughput'), default='paced')
    parser.add_argument('--input-fps', type=int, choices=range(1,241), help='rtmp: independently require this source cadence')
    parser.add_argument('--input-gop', type=int, help='rtmp: independently require this input I interval in frames')
    parser.add_argument('--save-output-dir', type=pathlib.Path, help='rtmp: also export verified compressed outputs to a new directory under out/ (outside timing)')
    parser.add_argument('--audit-burst-bits', type=int, help='rtmp network_trace: each audit bucket burst in bits (default 55000, zero initial credit)')
    parser.add_argument('--latency-observe-only',action='store_true',help='Experiment: retain latency measurements but do not reject candidates on latency cost')
    parser.add_argument('--build-dir', type=pathlib.Path)
    parser.add_argument('--bin-dir', type=pathlib.Path)
    parser.add_argument('--result-dir', type=pathlib.Path)
    parser.add_argument('--list', action='store_true')
    opts = parser.parse_args(argv)
    if not 1 <= opts.seconds <= 259200:
        parser.error('--seconds must be between 1 and 259200')
    if (opts.input_fps or opts.input_gop is not None) and opts.profile != 'rtmp':
        parser.error('--input-fps/--input-gop require rtmp')
    if opts.input_gop is not None and not 1 <= opts.input_gop <= 1000000:
        parser.error('--input-gop must be between 1 and 1000000')
    if opts.audit_burst_bits is not None and (opts.profile!='rtmp' or not 0<=opts.audit_burst_bits<=100000000):
        parser.error('--audit-burst-bits requires rtmp and 0..100000000')
    if opts.save_output_dir:
        opts.save_output_dir = opts.save_output_dir.resolve()
        if opts.profile != 'rtmp' or not opts.save_output_dir.is_relative_to((ROOT / 'out').resolve()) or opts.save_output_dir.exists():
            parser.error('--save-output-dir requires rtmp and a new directory under out/')
    if opts.profile == 'latency' and (opts.config or opts.repeats > 5):
        parser.error('latency accepts 1..5 repeats and uses its own generated fixture')
    if opts.profile in ('service', 'rtmp') and not opts.config:
        parser.error(opts.profile + ' requires --config')
    suite_id, script = {
        'latency': ('vvenc-latency', 'tests/benchmarks/vvenc_latency.py'),
        'service': ('service-capacity', 'tests/benchmarks/service_capacity.py'),
        'rtmp': ('rtmp-capacity', 'tests/benchmarks/rtmp_capacity.py'),
    }[opts.profile]
    options = ['full']
    for name in ('build_dir', 'bin_dir', 'result_dir'):
        if getattr(opts, name):
            options += ['--' + name.replace('_', '-'), str(getattr(opts, name))]
    args = runner.parse_args(options)
    args.profile, args.suite, args.include_native = 'benchmark', [suite_id], False
    suite = dict(id=suite_id, script=script, tools=['ffmpeg', 'ffprobe', 'pkg-config'])
    if opts.profile == 'rtmp':
        suite['binaries'] = ['naki-live-transport-fixture']
    command = [sys.executable, str(ROOT / script), '--repeats', str(opts.repeats)]
    metadata = dict(kind=opts.profile, repeats=opts.repeats,
                    boundary='Local service handler/ingress to actual mux/flush; excludes capture, upload and display.',
                    capacity_claim=False)
    if opts.profile in ('service', 'rtmp'):
        config = opts.config.resolve()
        value = json.loads(config.read_text())
        media = pathlib.Path(value['input']).expanduser()
        media = (media if media.is_absolute() else config.parent / media).resolve()
        if not media.is_file():
            parser.error(opts.profile + ' benchmark requires an existing local input file')
        config_hash = runner.digest(config)
        command += ['--config', str(config), '--config-sha256', config_hash,
                    '--seconds', str(opts.seconds), '--mode', opts.mode]
        metadata.update(config=str(config), config_sha256=config_hash, input=str(media),
                        mode=opts.mode, maximum_seconds=opts.seconds,
                        resources='Sampled /proc totals for controller and descendants; not cgroup/wire/display measurements.')
        if opts.profile == 'rtmp':
            if opts.latency_observe_only:command += ['--latency-observe-only']
            for key in ('input_fps', 'input_gop', 'audit_burst_bits'):
                if getattr(opts,key) is not None:
                    command += ['--' + key.replace('_','-'), str(getattr(opts,key))]
            if opts.save_output_dir:
                command += ['--save-output-dir', str(opts.save_output_dir)]
                metadata['save_output_dir'] = str(opts.save_output_dir)
            metadata.update(topology='Loopback RTMP source listener -> service pull/transcode/push -> RTMP capture listener',
                            expected_input_fps=opts.input_fps, expected_input_gop=opts.input_gop,
                            boundary='Service ingress/handler to actual output mux/flush; remote capture is independently decoded after timing. No source-to-screen measurement.')
            if value.get('network_trace'):
                metadata.update(network_trace=True,encoder_max_future_frames=value.get('encoder_max_future_frames',0),
                    boundary='Plain RTMP first media-message chunk TCP recv completion to actual output mux/flush; old ingress/handler metrics retained separately. Not wire/display time.',
                    target_audit=dict(enforcement=False,maxrate=value.get('maxrate'),burst_bits=opts.audit_burst_bits if opts.audit_burst_bits is not None else 55000,initial_credit_bits=0,deadline_ms=300))
    if opts.list:
        print(json.dumps(dict(benchmark=metadata, suite=suite, command=command, sdk_gate=True), indent=2))
        return 0
    commands = runner.commands_for(args, [])
    commands.append((suite_id, command, opts.repeats * (opts.seconds + 600) if opts.profile in ('service', 'rtmp') else opts.repeats * 180 + 120))
    return runner.execute_run(args, {'separate_gates': {}}, [suite], commands, metadata)


if __name__ == '__main__':
    raise SystemExit(main())
