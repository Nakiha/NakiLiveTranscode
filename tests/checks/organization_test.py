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
_spec = importlib.util.spec_from_file_location('architecture_guard', ROOT / 'scripts/check-architecture.py')
architecture_guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(architecture_guard)


class Organization(unittest.TestCase):
    def test_architecture(self):
        self.assertEqual(architecture_guard.check(ROOT), [])

    def test_architecture_rejects_boundary_violations(self):
        cases = [
            ('control/illegal.c', '#include <libavutil/sha.h>\n', 'native media SDK'),
            ('control/illegal.c', '#include "../worker/job.h"\n', 'private header'),
            ('controller/illegal.c', '#include <media/pipeline_internal.h>\n', 'private header'),
            ('media/illegal.c', '#include "mux_internal.h"\n', 'private header'),
            ('media/illegal.c', 'void f(void){av_write_frame(0,0);}', 'single-owner API'),
            ('media/illegal.c', 'void f(void){av_interleaved_write_frame(0,0);}', 'single-owner API'),
            ('media/illegal.c', '#include "pipeline_mux.c"\n', 'implementation include'),
            ('media/illegal.c', '#define H "mux_internal.h"\n#include H\n', 'computed include'),
            ('media/illegal.c', 'struct NakiMux { int secret; };', 'representation is private'),
            ('media/illegal.c', 'struct Job { int secret; };', 'representation is private'),
            ('media/illegal.c', 'void f(void){naki_job_frame_written(0,0);}', 'unauthorized Job publisher'),
            ('media/illegal.c', 'void f(void){naki_job_child_free(0);}', 'unauthorized Job publisher'),
        ]
        for name, source, expected in cases:
            with self.subTest(name=name, expected=expected), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                for item in ('worker/job.h', 'media/pipeline_internal.h', 'media/mux_internal.h', 'media/pipeline_mux.c'):
                    path = root / 'app' / item; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('')
                path = root / 'app' / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(source)
                (root / 'CMakeLists.txt').write_text('target_link_libraries(naki-control PUBLIC yyjson)')
                self.assertTrue(any(expected in error for error in architecture_guard.check(root)))

    def test_architecture_follows_wrappers_and_ignores_comments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            sources = {'controller/client.c': '#include "control/wrapper.h"\n',
                       'control/wrapper.h': '#include "worker/job.h"\n', 'worker/job.h': ''}
            for name, text in sources.items():
                path = root / 'app' / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text(text)
            (root / 'CMakeLists.txt').write_text('target_link_libraries(naki-control PUBLIC yyjson)')
            self.assertTrue(any('controller/client.c: transitive private state' in e for e in architecture_guard.check(root)))
            (root / 'app/control/wrapper.h').write_text('/* av_write_frame(0,0); */\n// #include "worker/job.h"\n')
            self.assertEqual(architecture_guard.check(root), [])
            (root / 'CMakeLists.txt').write_text('target_link_libraries(naki-control PUBLIC PkgConfig::FFMPEG)')
            self.assertTrue(any('links media SDK' in e for e in architecture_guard.check(root)))

    def test_all_control_sources_compile_without_media_sdk(self):
        subprocess.run(['cc', '-std=c11', '-Werror', '-fsyntax-only',
                        '-I' + str(ROOT / 'app'), '-I' + str(ROOT / 'third_party/yyjson/src'),
                        *map(str, (ROOT / 'app/control').glob('*.c'))],
                       capture_output=True, text=True, check=True)

    def test_media_contracts_cannot_access_mutable_job(self):
        prefix = pathlib.Path(os.environ.get('NAKI_BUILD_ROOT', pathlib.Path.home()/'.cache/naki-live-transcode-n9')) / 'install'
        command = ['cc', '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c',
                   '-I'+str(ROOT/'app'), '-I'+str(ROOT/'third_party/yyjson/src'), '-I'+str(prefix/'include'), '-']
        headers = '#include "worker/job_media.h"\n#include "worker/job_remote.h"\n'
        good = subprocess.run(command, input=headers+'int f(Job *j){return naki_job_config(j)->width;}\n', text=True, capture_output=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        for expression in ('sizeof(Job)', 'j->stats.packets_out', 'sizeof(j->mutex)'):
            bad = subprocess.run(command, input=headers+'int f(Job *j){return (int)('+expression+');}\n', text=True, capture_output=True)
            self.assertNotEqual(bad.returncode, 0, expression)

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
            self.assertEqual(listing['benchmark']['input'], str((directory / 'input.mp4').resolve()))
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
