#!/usr/bin/env python3
"""Reject self-consistent SDK B when the consumer independently expects A."""
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sdk_expected', ROOT / 'scripts/sdk-expected.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
sdk = gate.sdk


class ConsumerExpectationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sdk-expected-', dir=ROOT / 'out')
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.checkout = self.root / 'checkout'
        self.consumer = self.root / 'consumer'
        self.consumer.mkdir()
        for name in sdk.RECIPES:
            for root in (self.checkout, self.consumer):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, path)
        for relative in sdk.DEPENDENCIES.values():
            source = self.checkout / relative
            source.mkdir(parents=True)
            (source / 'marker.c').write_text('int sdk_marker(void) { return 1; }\n')
        self.prefix = self.root / 'cache/install'
        (self.prefix / 'include').mkdir(parents=True)
        (self.prefix / 'lib').mkdir()
        (self.prefix / 'include/test.h').write_text('int sdk_marker(void);\n')
        self.build_sdk(1)
        # Generated before inspecting or accepting the candidate installation.
        self.expected = gate.expected(self.checkout, pins={'ffmpeg': 'a' * 40, 'vvenc': 'b' * 40})
        self.expected_path = self.consumer / 'expected-sdk.json'
        sdk.atomic_json(self.expected_path, self.expected)

    def build_sdk(self, marker):
        source = self.checkout / sdk.DEPENDENCIES['ffmpeg'] / 'marker.c'
        source.write_text(f'int sdk_marker(void) {{ return {marker}; }}\n')
        subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o',
                        str(self.prefix / 'lib/libavformat.so')], check=True, capture_output=True)
        value = sdk.sources(self.checkout)
        value['artifacts'] = sdk.artifacts(self.prefix)
        value['object_keys'] = {name: sdk.object_key(value, name) for name in sdk.DEPENDENCIES}
        sdk.atomic_json(self.prefix / sdk.MANIFEST, value)

    def test_source_less_requires_expectation_before_compilation(self):
        with self.assertRaisesRegex(ValueError, 'requires an independent expected'):
            gate.verify(self.consumer, self.prefix)

    def test_expected_a_rejects_self_consistent_b(self):
        self.build_sdk(2)
        # B passes the former integrity-only consumer check, demonstrating the
        # actual boundary; no corruption or deliberately broken manifest needed.
        sdk.verify(self.consumer, self.prefix)
        with self.assertRaisesRegex(ValueError, 'does not match'):
            gate.verify(self.consumer, self.prefix, self.expected_path)

    def test_matching_a_and_source_checkout_pass(self):
        self.assertEqual(gate.verify(self.consumer, self.prefix, self.expected_path)['identity'],
                         self.expected['source_identity'])
        gate.verify(self.checkout, self.prefix)

    def test_official_consumer_rejects_b_before_cmake(self):
        for name in ('sdk-expected.py', 'build-service.sh'):
            shutil.copyfile(ROOT / 'scripts' / name, self.consumer / 'scripts' / name)
        yyjson = self.consumer / 'third_party/yyjson/src/yyjson.c'
        yyjson.parent.mkdir(parents=True); yyjson.write_text('/* gate fixture */\n')
        tools = self.root / 'gate-tools'; tools.mkdir()
        sentinel = self.root / 'cmake-invocations'
        for name, body in (('pkg-config', 'exit 0\n'), ('cmake', 'printf "called\\n" >> "$GATE_SENTINEL"\n')):
            tool = tools / name; tool.write_text('#!/bin/sh\n' + body); tool.chmod(0o755)
        env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                   NAKI_SKIP_FFMPEG='1', NAKI_BUILD_ROOT=str(self.prefix.parent),
                   NAKI_EXPECTED_SDK_MANIFEST=str(self.expected_path), GATE_SENTINEL=str(sentinel))
        command = ['bash', str(self.consumer / 'scripts/build-service.sh')]
        accepted = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertEqual(sentinel.read_text().splitlines(), ['called', 'called'])
        sentinel.unlink(); self.build_sdk(2)
        refused = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn('does not match', refused.stderr)
        self.assertFalse(sentinel.exists(), 'CMake must never run with SDK B')

    def test_matching_expectation_does_not_hide_artifact_corruption(self):
        with (self.prefix / 'lib/libavformat.so').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            gate.verify(self.consumer, self.prefix, self.expected_path)

    def test_sdk_cannot_supply_its_own_expectation(self):
        path = self.prefix / 'expected-sdk.json'
        shutil.copyfile(self.expected_path, path)
        with self.assertRaisesRegex(ValueError, 'candidate SDK'):
            gate.verify(self.consumer, self.prefix, path)

    def test_current_consumer_recipe_and_selected_key_are_authoritative(self):
        with self.assertRaisesRegex(ValueError, 'selected build input'):
            gate.verify(self.consumer, self.prefix, self.expected_path, '0' * 24)
        gate.verify(self.consumer, self.prefix, self.expected_path, self.expected['dependency_key'])
        (self.consumer / sdk.RECIPES[0]).write_text('a different current recipe\n')
        with self.assertRaisesRegex(ValueError, 'consumer checkout'):
            gate.verify(self.consumer, self.prefix, self.expected_path)

    def test_expected_platform_and_duplicate_keys_rejected(self):
        value = dict(self.expected, target=dict(gate.TARGET, architecture='aarch64'))
        sdk.atomic_json(self.expected_path, value)
        with self.assertRaisesRegex(ValueError, 'platform/schema'):
            gate.verify(self.consumer, self.prefix, self.expected_path)
        self.expected_path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            gate.verify(self.consumer, self.prefix, self.expected_path)

    def test_wrong_elf_architecture_even_with_resealed_artifacts(self):
        path = self.prefix / 'lib/libavformat.so'
        data = bytearray(path.read_bytes()); data[18:20] = (183).to_bytes(2, 'little'); path.write_bytes(data)
        value = json.loads((self.prefix / sdk.MANIFEST).read_text())
        value['artifacts'] = sdk.artifacts(self.prefix)
        sdk.atomic_json(self.prefix / sdk.MANIFEST, value)
        with self.assertRaisesRegex(ValueError, 'ELF target'):
            gate.verify(self.consumer, self.prefix, self.expected_path)


if __name__ == '__main__':
    unittest.main()
