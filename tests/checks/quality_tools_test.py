#!/usr/bin/env python3
"""Reject misleading PTS associations and incomplete native raw exports."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from support.service_harness import BIN

spec = importlib.util.spec_from_file_location('quality', ROOT / 'scripts/check-video-quality.py')
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


def frames(indices, base='1/60'):
    return [dict(pts=i+60, time_base=base, pict_type='I' if i % 120 == 0 else 'P') for i in indices]


class QualityTools(unittest.TestCase):
    def test_pts_alignment_keeps_actual_cancellation_tail_association(self):
        source = frames(range(140))
        indices = list(range(0, 126, 2)) + [128, 132]
        pairs = quality.align(source, [source[i] for i in indices], 2)
        self.assertEqual([p['source_index'] for p in pairs], indices)
        self.assertEqual(pairs[-1]['pts'], '16/5')

    def test_nearest_frame_or_rebased_pts_must_not_be_used(self):
        source = frames(range(40))
        for indices in ([0, 1, 4, 6], [2, 4, 6, 8], [0, 2, 2, 6], [0, 4, 2, 6]):
            with self.subTest(indices=indices), self.assertRaises(ValueError):
                quality.align(source, [source[i] for i in indices], 2)
        with self.assertRaises(ValueError):
            quality.align(source, [source[i] for i in (0, 2, 6, 8, 10, 12, 14, 16)], 2)

    def test_exact_rational_timebases_and_off_grid_i(self):
        source = frames(range(12))
        source[3]['pict_type'] = 'I'
        indices = [0, 2, 3, 4, 6, 8, 10]
        output = [dict(source[i], pts=(i+60)*50, time_base='1/3000') for i in indices]
        self.assertEqual([p['source_index'] for p in quality.align(source, output, 2)], indices)

    def test_native_export_requires_every_requested_frame_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory(prefix='quality-test-', dir=ROOT / 'out') as temporary:
            directory = pathlib.Path(temporary)
            media = directory / 'input.mp4'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=128x72:rate=30',
                            '-frames:v', '4', '-c:v', 'libx264', '-threads', '1', str(media)], check=True)
            selection, raw, manifest = [directory / name for name in ('indices.txt', 'reference.yuv', 'frames.json')]
            command = [str(BIN / 'naki-quality-video-export'), str(media), str(raw), str(selection), str(manifest), '64', '36', 'reference']
            selection.write_text('0\n2\n')
            subprocess.run(command, capture_output=True, check=True)
            self.assertEqual(raw.stat().st_size, 2*64*36*3)
            self.assertEqual([f['index'] for f in json.loads(manifest.read_text())], [0, 2])
            before = quality.digest(raw)
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(quality.digest(raw), before)
            # 8-bit decoded output is lifted exactly to the shared 10-bit
            # domain; it must not be resized or compared to a rounded reference.
            selection.write_text('0\n2\n')
            decoded_raw, decoded_manifest = directory/'decoded.yuv', directory/'decoded.json'
            decoded_command = [str(BIN/'naki-quality-video-export'), str(media), str(decoded_raw),
                               str(selection), str(decoded_manifest), '128', '72', 'decoded']
            subprocess.run(decoded_command, capture_output=True, check=True)
            expected = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(media), '-vf',
                "select='eq(n,0)+eq(n,2)',format=yuv420p10le", '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
            self.assertEqual(decoded_raw.read_bytes(), expected)
            # A truncated decode must fail rather than silently score fewer pairs.
            command[2], command[4] = str(directory / 'short.yuv'), str(directory / 'short.json')
            selection.write_text('0\n100\n')
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_save_output_flag_is_forwarded_without_creating_directory(self):
        with tempfile.TemporaryDirectory(prefix='quality-list-', dir=ROOT / 'out') as temporary:
            directory = pathlib.Path(temporary)
            (directory / 'input.mp4').write_bytes(b'list only')
            config = directory / 'config.json'
            config.write_text(json.dumps(dict(input='input.mp4')))
            saved = directory / 'saved'
            command = [sys.executable, str(ROOT / 'scripts/bench-service.py'), 'rtmp', '--config', str(config),
                       '--save-output-dir', str(saved), '--list']
            listing = json.loads(subprocess.check_output(command, text=True))
            self.assertIn(str(saved), listing['command'])
            self.assertFalse(saved.exists())
            saved.mkdir()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
