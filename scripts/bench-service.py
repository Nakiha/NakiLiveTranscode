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
    parser.add_argument('profile', choices=('latency', 'service'))
    parser.add_argument('--repeats', type=int, choices=range(1, 11), default=3)
    parser.add_argument('--config', type=pathlib.Path, help='Service JSON with a local input; outputs go into fresh evidence directories')
    parser.add_argument('--seconds', type=float, default=60, help='Maximum observation time per service run; EOF can end it sooner')
    parser.add_argument('--mode', choices=('paced', 'throughput'), default='paced')
    parser.add_argument('--build-dir', type=pathlib.Path)
    parser.add_argument('--bin-dir', type=pathlib.Path)
    parser.add_argument('--result-dir', type=pathlib.Path)
    parser.add_argument('--list', action='store_true')
    opts = parser.parse_args(argv)
    if not 1 <= opts.seconds <= 259200:
        parser.error('--seconds must be between 1 and 259200')
    if opts.profile == 'latency' and (opts.config or opts.repeats > 5):
        parser.error('latency accepts 1..5 repeats and uses its own generated fixture')
    if opts.profile == 'service' and not opts.config:
        parser.error('service requires --config')
    suite_id = 'vvenc-latency' if opts.profile == 'latency' else 'service-capacity'
    script = 'tests/benchmarks/vvenc_latency.py' if opts.profile == 'latency' else 'tests/benchmarks/service_capacity.py'
    options = ['full']
    for name in ('build_dir', 'bin_dir', 'result_dir'):
        if getattr(opts, name):
            options += ['--' + name.replace('_', '-'), str(getattr(opts, name))]
    args = runner.parse_args(options)
    args.profile, args.suite, args.include_native = 'benchmark', [suite_id], False
    suite = dict(id=suite_id, script=script, tools=['ffmpeg', 'ffprobe', 'pkg-config'])
    command = [sys.executable, str(ROOT / script), '--repeats', str(opts.repeats)]
    metadata = dict(kind=opts.profile, repeats=opts.repeats,
                    boundary='Local service handler/ingress to actual mux/flush; excludes capture, upload and display.',
                    capacity_claim=False)
    if opts.profile == 'service':
        config = opts.config.resolve()
        value = json.loads(config.read_text())
        media = pathlib.Path(value['input']).expanduser()
        media = (media if media.is_absolute() else config.parent / media).resolve()
        if not media.is_file():
            parser.error('service benchmark requires an existing local input file')
        config_hash = runner.digest(config)
        command += ['--config', str(config), '--config-sha256', config_hash,
                    '--seconds', str(opts.seconds), '--mode', opts.mode]
        metadata.update(config=str(config), config_sha256=config_hash, input=str(media),
                        mode=opts.mode, maximum_seconds=opts.seconds,
                        resources='Sampled /proc totals for controller and descendants; not cgroup/wire/display measurements.')
    if opts.list:
        print(json.dumps(dict(benchmark=metadata, suite=suite, command=command, sdk_gate=True), indent=2))
        return 0
    commands = runner.commands_for(args, [])
    commands.append((suite_id, command, opts.repeats * (opts.seconds + 600) if opts.profile == 'service' else opts.repeats * 180 + 120))
    return runner.execute_run(args, {'separate_gates': {}}, [suite], commands, metadata)


if __name__ == '__main__':
    raise SystemExit(main())
