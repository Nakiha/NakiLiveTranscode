#!/usr/bin/env python3
"""Exercise protocol byte capture, real I/O failures and CI evidence retention."""
import importlib.util
import json
import os
import pathlib
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from support.flv_capture import inspect_flv, receipt, verify_capture
from support.service_harness import BIN, ServiceHarness

spec = importlib.util.spec_from_file_location('collector', ROOT / 'scripts/collect-test-evidence.py')
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


def tag(kind, body):
    return bytes([kind]) + len(body).to_bytes(3, 'big') + bytes(7) + body + (11 + len(body)).to_bytes(4, 'big')


FLV = b'FLV\x01\x05\x00\x00\x00\x09' + bytes(4) + tag(9, b'\x91vvc1payload') + tag(8, b'\xaf\x01aac')


class Infrastructure(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='infra-fixture-', dir=ROOT / 'out')
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name)
        self.source = self.path / 'source.flv'
        self.source.write_bytes(FLV)

    def capture(self, source=None, limit=1048576, seconds=3, **kwargs):
        output = self.path / 'capture.flv'
        result = subprocess.run([str(BIN / 'naki-live-transport-fixture'), 'capture-flv',
                                 str(source or self.source), str(output), str(seconds), str(limit), '0'],
                                capture_output=True, text=True, timeout=10, **kwargs)
        return result, output

    def test_exact_bytes_and_independent_packet_counts(self):
        result, output = self.capture()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), FLV)
        transport = receipt(result.stdout, result.returncode)
        self.assertEqual(verify_capture(output, transport, 1, 1, True)['status'], 'verified_controlled_close')
        with self.assertRaisesRegex(ValueError, 'packet count'):
            verify_capture(output, transport, 2, 1, True)
        with self.assertRaisesRegex(ValueError, 'controlled stop'):
            verify_capture(output, transport, 1, 1, False)
        before = output.read_bytes()
        result, _ = self.capture()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), before)

    def test_truncation_and_tag_trailer_corruption(self):
        for data in (FLV[:-1], FLV[:-4] + bytes(4), FLV[:19], b'bad'):
            self.source.write_bytes(data)
            with self.assertRaises(ValueError):
                inspect_flv(self.source)

    def test_enhanced_late_aac_mp3_tracks_do_not_count_configuration_as_media(self):
        self.source.write_bytes(FLV + tag(8, b'\x95\x00mp4a\x01config') +
                                tag(8, b'\x95\x01mp4a\x01aac') +
                                tag(8, b'\x95\x01.mp3\x02mp3'))
        self.assertEqual(inspect_flv(self.source)['audio'], 3)
        for body in (b'\x95', b'\x95\x11mp4a\x01many', b'\x91opuspacket'):
            self.source.write_bytes(FLV + tag(8, body))
            with self.assertRaises(ValueError):
                inspect_flv(self.source)

    def test_limit_read_and_output_errors_fail(self):
        result, output = self.capture(limit=13)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('output_limit', result.stdout)
        output.unlink()
        result, output = self.capture(self.path / 'missing')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('open_error', result.stdout)
        output.unlink()
        self.source.write_bytes(FLV * 1024)
        def small_disk():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (128, 128))
        result, _ = self.capture(preexec_fn=small_disk)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout, 'write_error|flush_error|file_close_error')

    def test_stalled_network_is_failure(self):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen()
            def stalled():
                with server.accept()[0] as connection:
                    connection.sendall(FLV)
                    time.sleep(2)
            thread = threading.Thread(target=stalled)
            thread.start()
            result, _ = self.capture(f'tcp://127.0.0.1:{server.getsockname()[1]}', seconds=1)
            thread.join()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('deadline', result.stdout)

    def test_transport_errors_cannot_be_success_receipts(self):
        row = dict(event='capture_complete', status='failed', reason='read_error', bytes=100,
                   read_result=-5, close_result=0)
        for code in (0, 1, 12):
            with self.assertRaises(ValueError):
                receipt(json.dumps(row), code)
        row.update(status='unconfirmed', reason='rtmp_peer_close_unconfirmed')
        self.assertEqual(receipt(json.dumps(row), 12)['status'], 'unconfirmed')
        row['close_result'] = -32
        self.assertEqual(receipt(json.dumps(row), 12)['status'], 'unconfirmed')
        row['close_result'] = -12
        with self.assertRaises(ValueError):
            receipt(json.dumps(row), 12)
        with self.assertRaises(ValueError):
            receipt(json.dumps(row), 0)

    def test_inventory_keeps_new_suite_names_and_detects_missing_unsafe_evidence(self):
        root = self.path / 'out'
        root.mkdir()
        entries = []
        for name in ('checks-run', 'cqf-rate-new', 'read-origin-new', 'codec-phases-new'):
            path = root / name
            path.mkdir()
            (path / 'result.json').write_text('{"evidence":true}')
            entries.append(path)
        (entries[0] / 'summary.json').write_text(json.dumps(dict(steps=[dict(artifacts=[str(p) for p in entries[1:]])])))
        report = collector.collect(root, entries[:1], self.path / 'saved')
        self.assertEqual(report['status'], 'complete', report)
        self.assertEqual(len(report['files']), 5)
        (entries[1] / 'fixture-link').symlink_to(self.source)
        (entries[1] / 'sentinel.trace.000001.jsonl.active').write_text('unsealed')
        report = collector.collect(root, entries[:1], self.path / 'with-link')
        self.assertEqual(report['status'], 'complete')
        self.assertEqual(report['metadata'][0]['type'], 'symlink')
        self.assertTrue(any(row['type'] == 'unsealed_trace' for row in report['metadata']))
        self.assertFalse((self.path / 'with-link/out/cqf-rate-new/fixture-link').exists())
        self.assertFalse((self.path / 'with-link/out/cqf-rate-new/sentinel.trace.000001.jsonl.active').exists())
        (root / 'link').symlink_to(self.source)
        (entries[0] / 'summary.json').write_text(json.dumps(dict(steps=[dict(artifacts=[str(root / 'missing'), str(root / 'link'), str(self.source)])])))
        report = collector.collect(root, entries[:1], self.path / 'invalid')
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(len(report['errors']), 3)

    def test_failed_command_still_archives_outputs_and_preserves_exit(self):
        root = self.path / 'out'
        destination = root / 'results'
        program = "import pathlib,sys; p=pathlib.Path(sys.argv[1])/'new-suite'; p.mkdir(); (p/'failure.log').write_text('failed'); sys.exit(7)"
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/collect-test-evidence.py'),
                                 '--root', str(root), '--destination', str(destination), '--',
                                 sys.executable, '-c', program, str(root)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 7, result.stderr)
        report = json.loads(next(destination.glob('*/collection.json')).read_text())
        self.assertEqual(report['test_exit_code'], 7)
        self.assertEqual(report['status'], 'complete')
        self.assertTrue(report['files'][0]['sha256'])

    def test_array_summary_is_opaque_and_runner_references_are_collected(self):
        root = self.path / 'out'
        root.mkdir()
        runner, decoder, previous = [root / name for name in ('checks-new', 'decoder-new', 'previous')]
        for path in (runner, decoder, previous):
            path.mkdir()
        payload = json.dumps([dict(passed=True, steps=[dict(artifacts=[str(self.source)])])])
        (decoder / 'summary.json').write_text(payload)
        (decoder / 'frames.json').write_text('[0,1,2]')
        (previous / 'old.log').write_text('referenced existing evidence')
        (runner / 'summary.json').write_text(json.dumps(dict(steps=[dict(artifacts=[str(previous)])])))
        saved = self.path / 'saved-array'
        report = collector.collect(root, [runner, decoder], saved)
        self.assertEqual(report['status'], 'complete', report)
        self.assertEqual(len(report['files']), 4)
        self.assertEqual((saved / 'out/decoder-new/summary.json').read_text(), payload)
        self.assertTrue((saved / 'out/previous/old.log').is_file())

    def test_array_summary_cli_preserves_success_and_failed_command_exit(self):
        program = ("import json,pathlib,sys; p=pathlib.Path(sys.argv[1])/'decoder-new'; p.mkdir(); "
                   "(p/'summary.json').write_text(json.dumps([{'passed': True}])); sys.exit(int(sys.argv[2]))")
        for code in (0, 7):
            with self.subTest(code=code):
                root = self.path / f'out-{code}'
                destination = root / 'results'
                result = subprocess.run([sys.executable, str(ROOT / 'scripts/collect-test-evidence.py'),
                    '--root', str(root), '--destination', str(destination), '--',
                    sys.executable, '-c', program, str(root), str(code)], capture_output=True, timeout=15)
                self.assertEqual(result.returncode, code, result.stderr)
                report = json.loads(next(destination.glob('*/collection.json')).read_text())
                self.assertEqual(report['test_exit_code'], code)
                self.assertEqual(report['status'], 'complete', report)
                self.assertEqual(len(report['files']), 1)

    def test_malformed_runner_summary_fails_without_losing_raw_evidence(self):
        root = self.path / 'out'
        root.mkdir()
        entry = root / 'checks-new'
        entry.mkdir()
        for index, payload in enumerate([None, 'scalar', dict(steps=None), dict(steps=[1]), dict(steps=[dict(artifacts=[1])])]):
            with self.subTest(payload=payload):
                (entry / 'summary.json').write_text(json.dumps(payload))
                report = collector.collect(root, [entry], self.path / f'invalid-summary-{index}')
                self.assertEqual(report['status'], 'failed', report)
                self.assertTrue(report['errors'])
                self.assertEqual(len(report['files']), 1)

    def test_benchmark_strict_option_and_controller_credit_metadata(self):
        config = self.path / 'config.json'
        config.write_text(json.dumps(dict(input=str(self.source), network_trace=True,
            rate_control='constrained_crf', maxrate=1100000, source_rate_limit=True)))
        command = [sys.executable, str(ROOT / 'scripts/bench-service.py'), 'rtmp', '--config', str(config),
                   '--audit-burst-bits', '1100000', '--require-conformance', '--list']
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        listing = json.loads(result.stdout)
        audit = listing['benchmark']['target_audit']
        self.assertTrue(audit['controller_enforcement'])
        self.assertTrue(audit['require_conformance'])
        self.assertEqual(audit['initial_absolute_credit_bits'], 1100000)
        self.assertIn('--require-conformance', listing['command'])
        config.write_text(json.dumps(dict(input=str(self.source))))
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('needs network_trace', result.stderr)

    def test_partial_completion_is_reported_immediately_as_terminal(self):
        client = ServiceHarness()
        state = dict(state='completed_partial', error='branch queue protection')
        with patch.object(client, 'http', return_value=state) as http:
            with self.assertRaises(AssertionError) as error:
                client.finished(timeout=1)
            self.assertEqual(error.exception.args[0], state)
            self.assertEqual(http.call_count, 1)
            self.assertEqual(client.finished('completed_partial'), state)


if __name__ == '__main__':
    unittest.main()
