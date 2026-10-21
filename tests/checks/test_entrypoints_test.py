#!/usr/bin/env python3
"""Guard coverage migration and exercise real failing/timeout CTest commands."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import importlib.util
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('test_service', ROOT / 'scripts/test-service.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

# Frozen inventory of the former ci-test.sh, not inferred from the new catalog.
LEGACY = '''sdk_identity_test sdk_expected_test release_manifest_test sdk_build_integration
event_maintenance_test watermark_integration watermark_review_integration update_api_integration
multi_output_integration mixed_queue_integration audio_signature_integration video_rate_integration
sei_integration frame_policy_integration probe_integration decoder_integration service_integration
vvenc_integration vvenc_pts_integration live_mux_integration live_input_stability_integration
media_hooks_integration vvenc_latency observability_integration process_integration
control_lifecycle_integration health_progress_integration output_finalize_integration
crash_integration symbols_integration'''.split()


class Entrypoints(unittest.TestCase):
    def setUp(self):
        (ROOT / 'out').mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='entrypoints-test-', dir=ROOT / 'out')
        self.addCleanup(self.temporary.cleanup)
        self.directory = pathlib.Path(self.temporary.name)
        self.catalog = json.loads((ROOT / 'tests/service_suites.json').read_text())

    def test_full_preserves_every_former_ci_suite_once(self):
        suites = self.catalog['suites']
        actual = [pathlib.Path(row['script']).stem for row in suites if 'full' in row['profiles']]
        self.assertEqual(set(actual), set(LEGACY) | {'infrastructure_test', 'test_entrypoints_test', 'pipeline_lifecycle_integration', 'organization_test', 'cqf_rate_integration', 'quality_tools_test', 'live_budget_test', 'read_origin_integration', 'codec_phases_integration', 'codec_phase_tools_test'})
        self.assertEqual(len(actual), len(set(actual)))
        self.assertEqual(len({row['id'] for row in suites}), len(suites))
        for row in suites:
            self.assertTrue((ROOT / row['script']).is_file())
            for name in row.get('indirect', []) + row.get('sources', []):
                path = ROOT / name
                if row['id'] == 'decoder' and name in row.get('sources', []) and os.environ.get('NAKI_DECODER_PROBE_SOURCE'):
                    path = pathlib.Path(os.environ['NAKI_DECODER_PROBE_SOURCE']) / pathlib.Path(name).name
                self.assertTrue(path.is_file(), name)

    def test_helpers_and_external_gates_are_explicit(self):
        suites = {row['id']: row for row in self.catalog['suites']}
        self.assertIn('naki-watermark-state-test', suites['watermark-review']['binaries'])
        self.assertIn('naki-watermark-operation-test', suites['watermark-review']['binaries'])
        self.assertIn('naki-logger-test', suites['observability']['binaries'])
        self.assertIn('tests/support/probe_budget_integration.py', suites['probe']['indirect'])
        self.assertEqual(len(suites['live-input']['indirect']), 2)
        self.assertIn('tests/integration/docker_smoke.py', self.catalog['separate_gates'])
        self.assertIn('tests/checks/workflow_publication_test.py', self.catalog['separate_gates'])
        sanitized = {row['id'] for row in suites.values() if 'sanitize' in row['profiles']}
        self.assertEqual(sanitized, {'service', 'multi-output', 'watermark-review', 'lifecycle-media'})

    def test_selection_is_profile_bounded_and_preserves_catalog_order(self):
        args = runner.parse_args(['full', '--suite', 'live-mux', '--suite', 'service'])
        selected = runner.select_suites(args, self.catalog)
        self.assertEqual([s['id'] for s in selected], ['service', 'live-mux'])
        self.assertFalse(args.include_native)
        self.assertEqual([c[0] for c in runner.commands_for(args, selected)],
                         ['sdk-consumer', 'service', 'live-mux'])
        for argv in (['quick', '--suite', 'service'], ['full', '--suite', 'missing'],
                     ['full', '--suite', 'service', '--suite', 'service']):
            with self.assertRaises(ValueError):
                runner.select_suites(runner.parse_args(argv), self.catalog)
        args = runner.parse_args(['quick', '--suite', 'native'])
        self.assertEqual(runner.select_suites(args, self.catalog), [])
        self.assertEqual([c[0] for c in runner.commands_for(args, [])], ['native'])

    def test_selected_failure_is_not_reported_as_full_profile_success(self):
        run = self.directory / 'selected-failure'
        def failed(command, log, timeout, environment):
            log.write_text('deliberate selected-suite failure\n')
            return dict(command=command, exit_code=7, timed_out=False, elapsed_s=0, log=str(log))
        with patch.object(runner, 'preflight', return_value={'binaries': {}}), \
             patch.object(runner, 'run_step', side_effect=failed):
            code = runner.main(['quick', '--suite', 'event-maintenance', '--result-dir', str(run)])
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(summary['scope'], 'selected')
        self.assertEqual(summary['selected_suites'], ['event-maintenance'])
        self.assertEqual(summary['steps'][0]['exit_code'], 7)
        self.assertEqual(summary['status'], 'failed')

    def test_shared_support_and_suite_imports_do_not_create_fixtures(self):
        # A clean interpreter prevents cached imports from hiding side effects.
        program = '''
import http.server, ssl, urllib.request, tempfile, socket, subprocess, pathlib, sys
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path.cwd() / 'tests'))
import importlib.util
with patch.object(tempfile, 'mkdtemp', side_effect=AssertionError('import creates directory')), \
     patch.object(socket, 'socket', side_effect=AssertionError('import opens socket')), \
     patch.object(subprocess, 'Popen', side_effect=AssertionError('import starts process')), \
     patch.object(pathlib.Path, 'mkdir', side_effect=AssertionError('import creates directory')):
    for path in sorted(pathlib.Path('tests/support').glob('*.py')):
        if path.name == '__init__.py': continue
        __import__('support.' + path.stem)
    for path in sorted(pathlib.Path('tests/integration').glob('*_integration.py')):
        spec = importlib.util.spec_from_file_location(path.stem, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
'''
        subprocess.run([sys.executable, '-c', program], cwd=ROOT, check=True, capture_output=True, text=True)

    def test_complete_measurement_with_failed_conformance_propagates(self):
        run = self.directory / 'strict-benchmark'
        artifact = ROOT / 'out' / (self.directory.name + '-benchmark')
        self.addCleanup(lambda: __import__('shutil').rmtree(artifact, ignore_errors=True))
        def failed(command, log, timeout, environment):
            artifact.mkdir()
            (artifact / 'results.json').write_text(json.dumps(dict(
                status='failed', measurement_status='complete', conformance_status='failed')))
            log.write_text('measurement complete; target failed\n')
            return dict(command=command, exit_code=1, timed_out=False, elapsed_s=0, log=str(log))
        args = runner.parse_args(['quick', '--result-dir', str(run)])
        runner.select_suites(args, self.catalog)
        with patch.object(runner, 'preflight', return_value={'binaries': {}}), \
             patch.object(runner, 'run_step', side_effect=failed):
            code = runner.execute_run(args, self.catalog, [], [('rtmp-capacity', ['fixture'], 10)], {'kind': 'rtmp'})
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(summary['measurement_status'], 'complete')
        self.assertEqual(summary['conformance_status'], 'failed')
        self.assertEqual(summary['status'], 'failed')
        self.assertEqual(summary['steps'][0]['measurement_status'], 'complete')

    def test_actual_ctest_failures_and_missing_executables_propagate(self):
        source = self.directory / 'source'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(EntrypointFixture NONE)
enable_testing()
add_test(NAME failed COMMAND "${CMAKE_COMMAND}" -E false)
add_test(NAME missing COMMAND /nonexistent/naki-entrypoint-helper)
''')
        build = self.directory / 'build'
        subprocess.run(['cmake', '-S', str(source), '-B', str(build)], check=True, capture_output=True)
        for name in ('failed', 'missing'):
            result = runner.run_step(['ctest', '--test-dir', str(build), '-R', '^' + name + '$',
                                      '--output-on-failure'], self.directory / (name + '.log'), 15, dict(os.environ))
            self.assertNotEqual(result['exit_code'], 0, result)
            self.assertFalse(result['timed_out'])

    def test_timeout_is_failure(self):
        result = runner.run_step([sys.executable, '-c', 'import time; time.sleep(30)'],
                                 self.directory / 'timeout.log', .1, dict(os.environ))
        self.assertEqual(result['exit_code'], 124)
        self.assertTrue(result['timed_out'])

    def test_optional_not_run_is_preserved(self):
        (self.directory / 'COMPLETE.json').write_text(json.dumps({'status': 'passed', 'cases': {
            'https': {'status': 'not_run', 'reason': 'fixture not configured'}}}))
        cases = runner.unexecuted_cases([str(self.directory)])
        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0]['reason'], 'fixture not configured')

    def test_missing_dependency_fails_before_running_tests(self):
        args = argparse.Namespace(profile='full')
        with patch.object(runner.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'Missing required tool'):
                runner.preflight(args, [], self.catalog)

    def test_preflight_failure_still_writes_failed_evidence(self):
        run = self.directory / 'failed-run'
        with patch.object(runner, 'preflight', side_effect=ValueError('Missing required executable/helper: fixture')):
            result = runner.main(['quick', '--result-dir', str(run)])
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(result, 1)
        self.assertEqual(summary['status'], 'failed')
        self.assertTrue(summary['source_unchanged'])
        self.assertEqual(summary['steps'], [])

    def test_source_mutation_invalidates_a_run(self):
        run = self.directory / 'changed-run'
        before = {'sha256': 'before', 'files': {}}
        after = {'sha256': 'after', 'files': {}}
        with patch.object(runner, 'source_snapshot', side_effect=[before, after]), \
             patch.object(runner, 'preflight', side_effect=ValueError('fixture stop')):
            result = runner.main(['quick', '--result-dir', str(run)])
        summary = json.loads((run / 'summary.json').read_text())
        self.assertEqual(result, 1)
        self.assertFalse(summary['source_unchanged'])
        self.assertIn('Source inputs changed', summary['error'])

    def test_sanitizer_ci_gate_and_docker_wrapper_exist(self):
        workflow = (ROOT / '.github/workflows/service.yml').read_text()
        self.assertIn('bash scripts/test-sanitizers.sh', workflow)
        self.assertLess(workflow.index('name: Application and yyjson ASan UBSan checks'),
                        workflow.index('name: Package exact image'))
        docker = (ROOT / 'Dockerfile').read_text()
        self.assertIn('COPY tests/support/ tests/support/', docker)
        self.assertIn('COPY .github/ .github/', docker)
        self.assertIn('COPY Dockerfile compose.yaml ./', docker)
        self.assertIn('NAKI_EXPECTED_SDK_MANIFEST=/src/expected-sdk.json', docker)
        self.assertIn('NAKI_DECODER_PROBE_SOURCE=/src/tests/decoder-probes', docker)

    def test_docker_build_stage_contains_cmake_sources_and_local_headers(self):
        # Check the partial build stage, before the later test stage copies all
        # tests. A complete local checkout otherwise masks missing COPY inputs.
        docker = (ROOT / 'Dockerfile').read_text()
        stage = docker.split('FROM toolchain AS build\n', 1)[1].split('FROM build AS test\n', 1)[0]
        available = set()
        for line in stage.splitlines():
            if not line.startswith('COPY ') or line.startswith('COPY --from='):
                continue
            *sources, destination = shlex.split(line)[1:]
            for name in sources:
                source = ROOT / name
                target = pathlib.PurePosixPath(destination)
                if target.is_absolute():
                    continue
                if source.is_dir():
                    available.update((target / p.relative_to(source)).as_posix()
                                     for p in source.rglob('*') if p.is_file())
                else:
                    available.add((target / source.name if destination.endswith('/') else target).as_posix())
        required = set(re.findall(r'\b(?:app|tests|third_party)/[\w/.-]+\.(?:c|h)\b',
                                  (ROOT / 'CMakeLists.txt').read_text()))
        pending = list(required)
        while pending:
            name = pending.pop()
            source = ROOT / name
            self.assertTrue(source.is_file(), name)
            for header in re.findall(r'^\s*#include\s+"([^"]+)"', source.read_text(), re.M):
                # Local headers are resolved first beside the source, then app/.
                candidates = [source.parent / header, ROOT / 'app' / header]
                found = next((p for p in candidates if p.is_file()), None)
                if found:
                    relative = found.relative_to(ROOT).as_posix()
                    if relative not in required:
                        required.add(relative)
                        pending.append(relative)
        self.assertEqual(sorted(required - available), [], 'CMake inputs missing from Docker build stage')


if __name__ == '__main__':
    unittest.main()
