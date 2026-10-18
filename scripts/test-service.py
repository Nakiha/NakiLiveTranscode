#!/usr/bin/env python3
"""Run quick, full or selected ASan/UBSan checks with explicit local evidence."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'tests/service_suites.json'
PRODUCTION = ('naki-live', 'naki-live-worker', 'naki-live-ctl', 'naki-crash-collector')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_snapshot(root=ROOT):
    files = {}
    for name in ('app', 'scripts', 'tests', '.github', 'third_party/yyjson/src',
                 'CMakeLists.txt', 'Dockerfile', 'Dockerfile.ffmpeg', 'compose.yaml'):
        path = root / name
        paths = path.rglob('*') if path.is_dir() else [path]
        for item in sorted(paths):
            if item.is_file() and '__pycache__' not in item.parts and item.suffix != '.pyc':
                files[item.relative_to(root).as_posix()] = digest(item)
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'files': files}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def unexecuted_cases(artifacts):
    """Keep optional cases reported by existing suites visible at the top level."""
    rows = []

    def visit(value, location):
        if isinstance(value, dict):
            if value.get('status') in ('not_run', 'skipped'):
                rows.append({'case': location, 'reason': value.get('reason', 'See suite evidence')})
            for key, child in value.items():
                visit(child, location + '/' + key)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, location + '/' + str(index))

    for directory in artifacts:
        completion = pathlib.Path(directory) / 'COMPLETE.json'
        if completion.is_file():
            visit(json.loads(completion.read_text()), str(completion))
    return rows


def run_step(command, log, timeout, environment):
    """Bound the owned process group too, so a failed driver cannot leave workers."""
    began = time.monotonic()
    timed_out = False
    with log.open('wb') as stream:
        child = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=stream,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = child.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            timed_out = isinstance(error, subprocess.TimeoutExpired)
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            # The group may still contain children even if its leader exited.
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
            code = 124 if timed_out else 130
    return {'command': command, 'exit_code': code, 'timed_out': timed_out,
            'elapsed_s': time.monotonic() - began, 'log': str(log)}


def cache_values(build):
    values = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' in line and ':' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    return values


def preflight(args, selected, catalog):
    if platform.system() != 'Linux':
        raise ValueError('Service tests require Linux / WSL')
    if sys.version_info < (3, 11):
        raise ValueError('Python 3.11+ is required by SDK/source hashing')
    if os.environ.get('PYTHONOPTIMIZE') or sys.flags.optimize:
        raise ValueError('Python assertions must remain enabled')
    tools = {'bash', 'cmake', 'ctest'}
    for suite in selected:
        tools.update(suite.get('tools', []))
        for name in [suite['script'], *suite.get('indirect', []), *suite.get('sources', [])]:
            path = ROOT / name
            if suite['id'] == 'decoder' and name in suite.get('sources', []) and os.environ.get('NAKI_DECODER_PROBE_SOURCE'):
                path = pathlib.Path(os.environ['NAKI_DECODER_PROBE_SOURCE']) / pathlib.Path(name).name
            if not path.is_file():
                raise ValueError('Missing test source: ' + name)
    if args.profile != 'quick':
        tools.update(('ffmpeg', 'ffprobe', 'pkg-config'))
    for tool in sorted(tools):
        if not shutil.which(tool):
            raise ValueError('Missing required tool: ' + tool)
    values = cache_values(args.build_dir)
    if pathlib.Path(values.get('CMAKE_HOME_DIRECTORY', '')).resolve() != ROOT:
        raise ValueError('CMake build directory belongs to a different source checkout')
    sanitized = values.get('NAKI_SANITIZERS') == 'ON'
    if sanitized != (args.profile == 'sanitize'):
        raise ValueError('Use a separate NAKI_SANITIZERS=ON build for sanitize; normal profiles require OFF')
    if args.profile == 'full' and args.bin_dir != ROOT / 'out/bin':
        raise ValueError('Full suite currently requires out/bin: some legacy drivers use that fixed path')
    listing = subprocess.check_output(['ctest', '--test-dir', str(args.build_dir),
                                      '--show-only=json-v1'], text=True, timeout=30)
    tests = json.loads(listing)['tests']
    quick = []
    binary_files = set()
    for test in tests:
        labels = next((p['value'] for p in test['properties'] if p['name'] == 'LABELS'), [])
        if 'quick' in labels:
            quick.append(test['name'])
            # Native wrappers put their executable last; direct tests put it first.
            binary = pathlib.Path(test['command'][-1] if test['name'] in ('event-logger', 'logger-submit') else test['command'][0])
            if binary.parent.resolve() != args.bin_dir:
                raise ValueError('CTest binary directory differs from selected binaries: ' + str(binary))
            binary_files.add(binary)
    if sorted(quick) != sorted(catalog['native_quick']):
        raise ValueError('CTest quick inventory differs from tests/service_suites.json')
    binary_files.update(args.bin_dir / name for name in PRODUCTION)
    for suite in selected:
        binary_files.update(args.bin_dir / name for name in suite.get('binaries', []))
    for binary in sorted(binary_files):
        if not binary.is_file() or (binary.suffix != '.so' and not os.access(binary, os.X_OK)):
            raise ValueError('Missing required executable/helper: ' + str(binary))
    if args.profile != 'quick':
        prefix = args.cache / 'install'
        for name in ('bin/ffmpeg', 'bin/ffprobe', 'share/naki/ffmpeg-build.json'):
            if not (prefix / name).is_file():
                raise ValueError('Missing SDK input: ' + str(prefix / name))
    if any(suite.get('native_symbols') for suite in selected):
        for label, directory in (('FFmpeg', args.ffmpeg_symbols), ('VVenC', args.vvenc_symbols)):
            if not (directory / 'manifest.json').is_file():
                raise ValueError(f'Missing {label} symbol manifest: {directory}; set the corresponding --*-symbols option')
    return {'tools': {name: shutil.which(name) for name in sorted(tools)},
            'cmake_sanitizers': sanitized,
            'binaries': {str(path): digest(path) for path in sorted(binary_files)}}


def parse_args(argv=None):
    cache = pathlib.Path(os.environ.get('NAKI_BUILD_ROOT',
        pathlib.Path(os.environ.get('XDG_CACHE_HOME', pathlib.Path.home() / '.cache')) / 'naki-live-transcode-n9'))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=('quick', 'full', 'sanitize'))
    parser.add_argument('--list', action='store_true', help='Show direct and indirect coverage without running tests')
    parser.add_argument('--build-dir', type=pathlib.Path, default=os.environ.get('NAKI_SERVICE_BUILD_DIR', cache / 'service'))
    parser.add_argument('--bin-dir', type=pathlib.Path, default=os.environ.get('NAKI_SERVICE_BIN', ROOT / 'out/bin'))
    parser.add_argument('--ffmpeg-symbols', type=pathlib.Path, default=os.environ.get('NAKI_FFMPEG_SYMBOLS', '/symbols/ffmpeg'))
    parser.add_argument('--vvenc-symbols', type=pathlib.Path, default=os.environ.get('NAKI_VVENC_SYMBOLS', '/symbols/vvenc'))
    parser.add_argument('--result-dir', type=pathlib.Path, help='New directory under out/; must not already exist')
    args = parser.parse_args(argv)
    args.cache = cache.resolve()
    for name in ('build_dir', 'bin_dir', 'ffmpeg_symbols', 'vvenc_symbols'):
        setattr(args, name, pathlib.Path(getattr(args, name)).resolve())
    return args


def main(argv=None):
    args = parse_args(argv)
    catalog = json.loads(CATALOG.read_text())
    selected = [suite for suite in catalog['suites'] if args.profile in suite['profiles']]
    if args.list:
        print(json.dumps({'native_ctest_quick': catalog['native_quick'], 'suites': selected,
                          'separate_gates': catalog['separate_gates']}, indent=2))
        return 0
    out = ROOT / 'out'
    out.mkdir(exist_ok=True)
    if args.result_dir:
        run = args.result_dir.resolve()
        if not run.is_relative_to(out.resolve()) or run == out.resolve():
            raise ValueError('Results must use a new directory below out/')
        run.mkdir(parents=True, exist_ok=False)
    else:
        run = pathlib.Path(tempfile.mkdtemp(prefix='checks-' + args.profile + '-', dir=out))
    print('Test evidence:', run, flush=True)
    before = source_snapshot()
    write_json(run / 'source-before.json', before)
    summary = {'schema_version': 1, 'profile': args.profile, 'status': 'running',
               'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'source_sha256': before['sha256'], 'host': platform.platform(),
               'build_dir': str(args.build_dir), 'bin_dir': str(args.bin_dir),
               'steps': [], 'separate_gates': catalog['separate_gates'],
               'instrumentation': 'Application/yyjson/test helpers only; native SDK and system libraries are not instrumented. Deliberate fatal/symbol suites run in full, not sanitize.' if args.profile == 'sanitize' else 'Normal build'}
    summary['source_revision'] = os.environ.get('NAKI_SOURCE_REVISION')
    if (ROOT / '.git').exists() and shutil.which('git'):
        summary['source_revision'] = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True, timeout=30).strip()
    summary['sanitizer_options'] = {key: os.environ.get(key) for key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS')}
    write_json(run / 'summary.json', summary)
    environment = dict(os.environ, NAKI_BUILD_ROOT=str(args.cache),
                       NAKI_SERVICE_BIN=str(args.bin_dir), PYTHONUNBUFFERED='1')
    try:
        summary['preflight'] = preflight(args, selected, catalog)
        prefix = args.cache / 'install'
        if (prefix / 'share/naki/ffmpeg-build.json').is_file():
            manifest = prefix / 'share/naki/ffmpeg-build.json'
            summary['sdk'] = {'manifest_sha256': digest(manifest),
                              'identity': json.loads(manifest.read_text())['identity']}
        commands = []
        if args.profile != 'quick':
            verify = [sys.executable, str(ROOT / 'scripts/sdk-expected.py'), 'verify', '--prefix', str(prefix)]
            commands.append(('sdk-consumer', verify, 120))
        commands.append(('native', ['ctest', '--test-dir', str(args.build_dir), '-L', 'quick',
                                    '--output-on-failure', '--no-tests=error'], 240))
        for suite in selected:
            command = [sys.executable, str(ROOT / suite['script']), *suite.get('args', [])]
            if suite.get('native_symbols'):
                command.extend(['--ffmpeg-symbols', str(args.ffmpeg_symbols), '--vvenc-symbols', str(args.vvenc_symbols)])
            commands.append((suite['id'], command, suite.get('timeout_s', 300)))
        for index, (name, command, timeout) in enumerate(commands, 1):
            print(f'[{index}/{len(commands)}] {name}', flush=True)
            previous = set(out.iterdir())
            result = run_step(command, run / f'{index:02d}-{name}.log', timeout, environment)
            artifacts = [str(path) for path in sorted(set(out.iterdir()) - previous)]
            result.update(id=name, artifacts=artifacts, not_run=unexecuted_cases(artifacts))
            summary['steps'].append(result)
            write_json(run / 'summary.json', summary)
            if result['exit_code']:
                tail = pathlib.Path(result['log']).read_text(errors='replace').splitlines()[-35:]
                print('\n'.join(tail), flush=True)
                raise ValueError(f'{name} failed with exit {result["exit_code"]}; see {result["log"]}')
        summary['status'] = 'passed'
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        summary['status'] = 'failed'
        summary['error'] = str(error)
        print('FAIL:', error, file=sys.stderr, flush=True)
    finally:
        after = source_snapshot()
        write_json(run / 'source-after.json', after)
        summary['source_unchanged'] = before == after
        if not summary['source_unchanged']:
            summary.update(status='failed', error='Source inputs changed during the run')
        if 'preflight' in summary:
            summary['binaries_unchanged'] = all(pathlib.Path(name).is_file() and digest(pathlib.Path(name)) == value
                for name, value in summary['preflight']['binaries'].items())
            if not summary['binaries_unchanged']:
                summary.update(status='failed', error='Selected binaries changed during the run')
        if 'sdk' in summary:
            manifest = args.cache / 'install/share/naki/ffmpeg-build.json'
            summary['sdk_manifest_unchanged'] = manifest.is_file() and digest(manifest) == summary['sdk']['manifest_sha256']
            if not summary['sdk_manifest_unchanged']:
                summary.update(status='failed', error='SDK manifest changed during the run')
        write_json(run / 'summary.json', summary)
    summary['not_run'] = [dict(case, suite=step['id']) for step in summary['steps'] for case in step.get('not_run', [])]
    write_json(run / 'summary.json', summary)
    print(summary['status'].upper() + ': ' + str(run / 'summary.json'), flush=True)
    if summary['not_run']:
        print('Explicit optional cases not run:', len(summary['not_run']), '(see summary)', flush=True)
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit('Test entry point failed: ' + str(error))
