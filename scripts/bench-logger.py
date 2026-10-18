#!/usr/bin/env python3
"""Compare a saved logger source pair with the working tree in fresh processes.

This is an instrumented CPU/queue microbenchmark with a memory-only stdout
callback. Caller document construction is outside timed submit calls. Wall
throughput and whole-process peak RSS include the harness and caller documents.
Mutex wait/hold sums include every acquisition in one submit (two when admitted
by the candidate). It measures neither disk performance nor media latency.
"""
import argparse
import hashlib
import json
import pathlib
import platform
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dir', required=True, type=pathlib.Path)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error('repeats must be 1..10')
    out = ROOT / 'out'
    out.mkdir(exist_ok=True)
    run = pathlib.Path(tempfile.mkdtemp(prefix='logger-bench-', dir=out))
    sources = {'before': args.baseline_dir.resolve(), 'after': ROOT / 'app/observability'}
    evidence = {'boundary': __doc__, 'host': platform.platform(),
                'compiler': subprocess.check_output(['cc', '--version'], text=True),
                'sources': {}, 'builds': [], 'runs': []}
    common = [ROOT / 'tests/benchmarks/logger_submit_bench.c', ROOT / 'app/crash/crash_client.c', ROOT / 'app/crash/crash.h',
              ROOT / 'third_party/yyjson/src/yyjson.c', ROOT / 'third_party/yyjson/src/yyjson.h']
    evidence['common_sources'] = {str(p): digest(p) for p in common}
    for label, source in sources.items():
        evidence['sources'][label] = {str(source / name): digest(source / name) for name in ('logger.c', 'logger.h')}
        command = ['cc', '-O2', '-g', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I' + str(source), '-I' + str(ROOT / 'app'), '-I' + str(ROOT / 'app/crash'),
                   '-I' + str(ROOT / 'app/observability'), '-I' + str(ROOT / 'third_party/yyjson/src'),
                   str(common[0]), str(source / 'logger.c'), str(common[1]), str(common[3]),
                   '-Wl,--wrap=pthread_mutex_lock', '-Wl,--wrap=pthread_mutex_unlock', '-o', str(run / label)]
        result = subprocess.run(command, text=True, capture_output=True)
        (run / (label + '-build.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        evidence['builds'].append({'command': command, 'sha256': digest(run / label)})
    # Interleave old/new, reversing their order every repetition to reduce drift.
    for threads, count, size, blocked in ((1,2000,128,0),(4,2000,128,0),(8,200,8192,0),(8,2000,8192,0),(4,2000,8192,1)):
        for repeat in range(args.repeats):
            for label in (('before','after') if repeat % 2 == 0 else ('after','before')):
                command = [str(run / label), str(threads), str(count), str(size), str(blocked)]
                result = subprocess.run(command, check=True, text=True, capture_output=True, timeout=30)
                row = json.loads(result.stdout)
                row.update(variant=label, repeat=repeat + 1, command=command)
                evidence['runs'].append(row)
                (run / 'summary.json').write_text(json.dumps(evidence, indent=2) + '\n')
    for label, source in sources.items():
        assert evidence['sources'][label] == {str(source / name): digest(source / name) for name in ('logger.c','logger.h')}
    assert evidence['common_sources'] == {str(p): digest(p) for p in common}
    evidence['inputs_unchanged'] = True
    (run / 'summary.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(run / 'summary.json')


if __name__ == '__main__':
    main()
