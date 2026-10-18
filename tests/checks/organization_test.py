#!/usr/bin/env python3
"""Keep controller/config boundaries and standalone benchmark invocation usable."""
import ast
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class Organization(unittest.TestCase):
    def test_no_imports_from_old_flat_test_layout_inside_functions(self):
        # Walk the whole AST, including function-local imports that a plain
        # import smoke cannot execute.
        local_names = {p.stem for p in (ROOT / 'tests').rglob('*.py')}
        for path in (ROOT / 'tests').rglob('*.py'):
            for node in ast.walk(ast.parse(path.read_text())):
                if isinstance(node, ast.Import):
                    for alias in node.names:
                        self.assertNotIn(alias.name, local_names, str(path))
                elif isinstance(node, ast.ImportFrom) and not node.level:
                    self.assertNotIn(node.module, local_names, str(path))

    def test_config_and_metrics_headers_need_no_native_sdk(self):
        # No pkg-config or SDK include paths: these contracts are plain C/yyjson.
        code = '#include "control/config.h"\n#include "observability/metrics.h"\nint main(void){Config c={0};Metric m={0};return c.output_count+(int)m.count;}\n'
        subprocess.run(['cc', '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c',
                        '-I' + str(ROOT / 'app'), '-I' + str(ROOT / 'third_party/yyjson/src'), '-'],
                       input=code, text=True, capture_output=True, check=True)

    def test_controller_cannot_include_worker_mutable_state(self):
        pending = list((ROOT / 'app/controller').glob('*.[ch]'))
        seen = set()
        while pending:
            path = pending.pop()
            if path in seen:
                continue
            seen.add(path)
            self.assertNotEqual(path, ROOT / 'app/worker/job.h')
            self.assertNotEqual(path, ROOT / 'app/media/pipeline_internal.h')
            for name in re.findall(r'^\s*#include\s+"([^"]+)"', path.read_text(), re.M):
                child = next((p for p in (path.parent / name, ROOT / 'app' / name) if p.is_file()), None)
                if child:
                    pending.append(child)

    def test_every_native_test_is_inside_build_testing(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        before = cmake.split('if(BUILD_TESTING)', 1)[0]
        self.assertNotRegex(before, r'add_(?:executable|library)\([^)]*tests/')

    def test_benchmark_list_works_from_another_directory_and_binds_input(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            (directory / 'input.mp4').write_bytes(b'list-only fixture')
            config = directory / 'config.json'
            config.write_text(json.dumps(dict(input='input.mp4', output='ignored.ts')))
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/bench-service.py'), 'service',
                '--config', str(config), '--list'], cwd=directory, capture_output=True, text=True, check=True)
            listing = json.loads(result.stdout)
            self.assertEqual(listing['benchmark']['input'], str(directory / 'input.mp4'))
            self.assertTrue(listing['sdk_gate'])
            self.assertFalse(listing['benchmark']['capacity_claim'])
            bad = subprocess.run([sys.executable, str(ROOT / 'scripts/bench-service.py'), 'service',
                '--config', str(config), '--seconds', 'nan', '--list'], capture_output=True)
            self.assertNotEqual(bad.returncode, 0)

    def test_benchmark_redirects_each_fixed_output_without_mutating_template(self):
        spec = importlib.util.spec_from_file_location('capacity', ROOT / 'tests/benchmarks/service_capacity.py')
        capacity = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(capacity)
        for count in (1, 2, 4):
            original = dict(input='/tmp/input.mp4', outputs=[
                dict(id=f'out-{i}', output='srt://unused', output_format='mpegts', encoder_threads=2)
                for i in range(count)])
            resolved = capacity.resolved_config(original, pathlib.Path('/tmp/unique-run'), 'throughput')
            self.assertFalse(resolved['pace'])
            self.assertEqual(len({o['output'] for o in resolved['outputs']}), count)
            for before, after in zip(original['outputs'], resolved['outputs']):
                self.assertEqual(before['output'], 'srt://unused')
                self.assertEqual(after['encoder_threads'], 2)
                self.assertTrue(after['output'].startswith('/tmp/unique-run/'))


if __name__ == '__main__':
    unittest.main()
