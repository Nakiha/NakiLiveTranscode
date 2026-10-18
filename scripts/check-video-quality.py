#!/usr/bin/env python3
"""Offline PSNR/VMAF for a completed RTMP benchmark; saves the compressed output.

Native SDK decode/filter creates both raw streams. The separate metrics FFmpeg
needs rawvideo, psnr and libvmaf; it never changes the measured service SDK.
"""
import argparse
import fractions
import hashlib
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from support.service_harness import BIN, CACHE, PROBE


def digest(path):
    with pathlib.Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def align(source, output, divisor):
    """Exact rational source PTS association; never framesync a nearby picture."""
    def stamps(frames):
        values = [fractions.Fraction(f['pts']) * fractions.Fraction(f['time_base']) for f in frames]
        if any(b <= a for a, b in zip(values, values[1:])):
            raise ValueError('Missing, duplicate or nonmonotonic decoded PTS')
        return values
    source_pts, output_pts = stamps(source), stamps(output)
    selected = {pts: i for i, pts in enumerate(source_pts)
                if i % divisor == 0 or source[i]['pict_type'] == 'I'}
    pairs = []
    for index, pts in enumerate(output_pts):
        if pts not in selected:
            raise ValueError(f'Output PTS {pts} has no exactly matching selected source frame')
        pairs.append(dict(output_index=index, source_index=selected[pts], pts=str(pts)))
    if not pairs or pairs[0]['source_index'] != 0:
        raise ValueError('Output must include the first selected source frame')
    # The benchmark explicitly permits cancellation of at most the last short
    # B group. Tail pictures are still scored against their own source PTS.
    expected = list(selected.values())
    prefix = max(0, len(pairs) - 4)
    if [p['source_index'] for p in pairs[:prefix]] != expected[:prefix]:
        raise ValueError('Output prefix skips selected source frames')
    return pairs


def probe(path, directory, name):
    command = [str(PROBE), '-v', 'error', '-threads', '2', '-select_streams', 'v:0',
               '-show_frames', '-show_streams', '-show_entries',
               'frame=pts,pict_type:stream=codec_name,width,height,pix_fmt,time_base,color_range,color_space,color_transfer,color_primaries',
               '-of', 'json', str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    (directory / (name + '.stderr')).write_text(result.stderr)
    (directory / (name + '.json')).write_text(result.stdout)
    if result.returncode or result.stderr.strip():
        raise RuntimeError('Independent quality probe failed: ' + result.stderr)
    value = json.loads(result.stdout)
    stream, = value['streams']
    frames = value['frames']
    if not 1 <= len(frames) <= 250000:
        raise ValueError('Quality check requires 1..250000 decoded frames')
    for frame in frames:
        frame['time_base'] = stream['time_base']
    return stream, frames


def metric_path(path, executable):
    # WSL can use an existing Windows analysis tool without installing filters
    # into the pinned service SDK. Files remain on a Windows-mounted workspace.
    if sys.platform == 'linux' and executable.suffix.lower() == '.exe':
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip()
    return str(path.resolve())


def metrics(executable, distorted, reference, width, height, fps, directory, frames, threads):
    directory.mkdir(exist_ok=True)
    command = [str(executable), '-nostdin', '-hide_banner', '-v', 'info', '-filter_complex_threads', '1']
    for path in (distorted, reference):
        command += ['-f', 'rawvideo', '-pixel_format', 'yuv420p10le', '-video_size', f'{width}x{height}',
                    '-framerate', str(fps), '-i', metric_path(path, executable)]
    graph = ('[0:v]split=2[d0][d1];[1:v]split=2[r0][r1];'
             '[d0][r0]psnr=stats_file=psnr.log:stats_version=2:output_max=1:shortest=1:repeatlast=0[p];'
             '[d1][r1]libvmaf=log_fmt=json:log_path=vmaf.json:model=version=vmaf_v0.6.1:'
             f'n_threads={threads}:n_subsample=1:shortest=1:repeatlast=0[v]')
    command += ['-filter_complex', graph, '-map', '[p]', '-map', '[v]', '-f', 'null', '-']
    write_json(directory / 'command.json', command)
    with (directory / 'ffmpeg.log').open('w') as log:
        subprocess.run(command, cwd=directory, stdout=log, stderr=log, check=True, timeout=7200)
    scores = json.loads((directory / 'vmaf.json').read_text())
    psnr = [dict(field.split(':', 1) for field in line.split())
            for line in (directory / 'psnr.log').read_text().splitlines() if line.startswith('n:')]
    if len(scores['frames']) != frames or len(psnr) != frames:
        raise RuntimeError('Metric frame count differs from exact PTS pair count')
    if [f['frameNum'] for f in scores['frames']] != list(range(frames)) or [int(f['n']) for f in psnr] != list(range(1, frames+1)):
        raise RuntimeError('Metric frame numbering is incomplete')
    match = re.search(r'PSNR y:(\S+) u:(\S+) v:(\S+) average:(\S+) min:(\S+) max:(\S+)',
                      (directory / 'ffmpeg.log').read_text())
    if not match:
        raise RuntimeError('Missing PSNR pooled summary')
    # JSON strings preserve infinite PSNR without writing nonstandard Infinity.
    pooled = {name: ('inf' if value == 'inf' else float(value)) for name, value in
              zip(('y_db', 'u_db', 'v_db', 'average_db', 'min_frame_db', 'max_frame_db'), match.groups())}
    values = sorted(f['metrics']['vmaf'] for f in scores['frames'])
    if any(not math.isfinite(v) for v in values):
        raise RuntimeError('Nonfinite VMAF')
    return dict(frames=frames, psnr=pooled, vmaf=dict(scores['pooled_metrics']['vmaf'],
                p05=values[max(0, math.ceil(frames*.05)-1)], p50=values[math.ceil(frames*.5)-1]),
                libvmaf_version=scores.get('version'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark-run', type=pathlib.Path, required=True)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--metrics-ffmpeg', type=pathlib.Path, required=True)
    parser.add_argument('--result-dir', type=pathlib.Path)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--keep-raw', action='store_true', help='Retain both packed 10-bit raw streams (large)')
    args = parser.parse_args()
    if not 1 <= args.threads <= 64 or not 1 <= args.repeat <= 10:
        parser.error('--threads must be 1..64 and --repeat 1..10')
    benchmark = args.benchmark_run.resolve()
    completed = json.loads((benchmark / 'COMPLETE.json').read_text())
    if completed['status'] != 'passed' or args.repeat > len(completed['results']):
        parser.error('Requires a passed RTMP run and an existing repeat')
    row = completed['results'][args.repeat-1]
    source, captured = benchmark / 'source.flv', benchmark / str(args.repeat) / 'captured.flv'
    config_path = benchmark / str(args.repeat) / 'resolved-config.json'
    config = json.loads(config_path.read_text())
    if config.get('encoder') != 'libvvenc' or config.get('intra', {}).get('mode') != 'follow_source' or config.get('watermark') or config.get('watermarks'):
        parser.error('Quality reference supports fixed libvvenc follow_source with no watermark')
    if digest(source) != completed['source_flv_sha256'] or digest(captured) != row['output_sha256']:
        parser.error('Benchmark media changed since measurement')
    result = args.result_dir.resolve() if args.result_dir else None
    if result and not result.is_relative_to((ROOT / 'out').resolve()):
        parser.error('--result-dir must be a new directory under out/')
    (ROOT / 'out').mkdir(exist_ok=True)
    if result:
        result.mkdir(parents=True, exist_ok=False)
    else:
        result = pathlib.Path(tempfile.mkdtemp(prefix='video-quality-', dir=ROOT / 'out'))
    evidence = dict(status='running', benchmark_run=str(benchmark), repeat=args.repeat,
                    boundary='Offline coding distortion against decoded 1080p input, with exact selected PTS and the service bilinear 540p 10-bit filter. Excludes 4K-to-1080p preparation loss, 60-to-30 temporal loss, audio and display.',
                    model='vmaf_v0.6.1', metric_resolution='native output', n_subsample=1,
                    pixel_format='yuv420p10le', psnr_peak=1023,
                    reference_filter='app/media/video_filter.c:naki_video_filter_prepare',
                    raw_retained=args.keep_raw)
    inputs = [source, captured, config_path, benchmark / 'COMPLETE.json', PROBE,
              BIN / 'naki-quality-video-export', args.metrics_ffmpeg.resolve(),
              pathlib.Path(__file__).resolve(), ROOT / 'tests/fixtures/quality_video_export.c',
              ROOT / 'app/media/video_filter.c', ROOT / 'app/media/video_filter.h',
              CACHE / 'install/share/naki/ffmpeg-build.json']
    try:
        evidence['input_hashes'] = {str(p): digest(p) for p in inputs}
        with (result / 'sdk-verify.log').open('w') as log:
            subprocess.run([sys.executable, str(ROOT / 'scripts/sdk-expected.py'), 'verify', '--prefix', str(CACHE / 'install')],
                           stdout=log, stderr=log, check=True, timeout=180)
        executable = args.metrics_ffmpeg.resolve()
        version = subprocess.check_output([str(executable), '-version'], stderr=subprocess.STDOUT, text=True)
        (result / 'metrics-version.txt').write_text(version)
        shutil.copyfile(captured, result / 'output.flv')
        evidence['saved_output'] = str(result / 'output.flv')
        evidence['saved_output_sha256'] = digest(result / 'output.flv')
        source_stream, source_frames = probe(source, result, 'source-probe')
        output_stream, output_frames = probe(captured, result, 'output-probe')
        if len(output_frames) != row['packets_out']:
            raise ValueError('Quality decode count differs from benchmark')
        for stream in (source_stream, output_stream):
            if stream.get('color_transfer') not in ('bt709', 'smpte170m', 'bt470bg') or stream.get('color_range') != 'tv':
                raise ValueError('Quality v1 requires explicitly tagged limited-range SDR')
        expected_format = 'yuv420p' if config.get('encoder_bit_depth', 10) == 8 else 'yuv420p10le'
        if output_stream['pix_fmt'] != expected_format or any(source_stream.get(k) != output_stream.get(k) for k in ('color_space', 'color_transfer', 'color_primaries')):
            raise ValueError('Unsupported output format or color metadata mismatch')
        evidence['encoder_bit_depth'] = config.get('encoder_bit_depth', 10)
        evidence['encoder_input_bit_depth'] = config.get('encoder_input_bit_depth', 10)
        evidence['decoded_output_pixel_format'] = output_stream['pix_fmt']
        evidence['distorted_normalization'] = '8-bit decoded samples lifted to 10-bit by the service format filter' if expected_format == 'yuv420p' else 'none'
        pairs = align(source_frames, output_frames, completed['decimation_divisor'])
        write_json(result / 'alignment.json', pairs)
        width, height = output_stream['width'], output_stream['height']
        evidence.update(width=width, height=height, frames=len(pairs), source_frames=len(source_frames),
                        first_pts=pairs[0]['pts'], last_pts=pairs[-1]['pts'],
                        cancellation_tail_policy='All decoded output frames scored by their own exact PTS; no repeated reference tail',
                        source_stream=source_stream, output_stream=output_stream)
        raw_files = []
        for name, media, indices, mode in (
            ('reference', source, [p['source_index'] for p in pairs], 'reference'),
            ('distorted', captured, [p['output_index'] for p in pairs], 'decoded')):
            selection = result / (name + '-indices.txt')
            selection.write_text(''.join(str(i) + '\n' for i in indices))
            raw = result / (name + '.yuv')
            manifest = result / (name + '-frames.json')
            command = [str(BIN / 'naki-quality-video-export'), str(media), str(raw), str(selection), str(manifest), str(width), str(height), mode]
            write_json(result / (name + '-command.json'), command)
            with (result / (name + '-export.log')).open('w') as log:
                subprocess.run(command, stdout=log, stderr=log, check=True, timeout=900)
            exported = json.loads(manifest.read_text())
            if len(exported) != len(pairs) or raw.stat().st_size != len(pairs)*width*height*3:
                raise RuntimeError('Raw frame count/size mismatch')
            for item, index, pair in zip(exported, indices, pairs):
                if item['index'] != index or fractions.Fraction(item['pts']*item['tb_num'], item['tb_den']) != fractions.Fraction(pair['pts']):
                    raise RuntimeError('Raw exporter changed frame/PTS association')
            evidence[name + '_raw'] = dict(bytes=raw.stat().st_size, sha256=digest(raw))
            raw_files.append(raw)
        ref_manifest = json.loads((result / 'reference-frames.json').read_text())
        dst_manifest = json.loads((result / 'distorted-frames.json').read_text())
        if any((a['colorspace'], a['range']) != (b['colorspace'], b['range']) for a, b in zip(ref_manifest, dst_manifest)):
            raise RuntimeError('Raw reference/output color metadata differs')
        # Identical-stream guard checks plane packing and metric wiring. Exact
        # PTS association is checked independently above and in unit coverage.
        identity = result / 'identity.yuv'
        check_frames = min(16, len(pairs))
        with raw_files[1].open('rb') as src, identity.open('xb') as dst:
            dst.write(src.read(check_frames*width*height*3))
        check = metrics(executable, identity, identity, width, height, config.get('output_fps', 30), result / 'identity-check', check_frames, args.threads)
        evidence['identity_check'] = check
        if check['psnr']['average_db'] != 'inf' or check['vmaf']['mean'] < 99:
            raise RuntimeError('Identical-stream metric guard failed')
        evidence['metrics'] = metrics(executable, raw_files[1], raw_files[0], width, height,
                                       config.get('output_fps', 30), result / 'metrics', len(pairs), args.threads)
        if any(digest(p) != sha for p, sha in evidence['input_hashes'].items()) or digest(result / 'output.flv') != row['output_sha256']:
            raise RuntimeError('Inputs/tools/source changed during quality check')
        evidence['status'] = 'passed'
        if not args.keep_raw:
            for path in [*raw_files, identity]:
                path.unlink()  # Only this run's explicitly generated raw intermediates.
        write_json(result / 'COMPLETE.json', evidence)
        print('PASS video quality:', result, flush=True)
        print(json.dumps(evidence['metrics'], indent=2))
    except BaseException as error:
        evidence.update(status='failed', error=str(error), raw_retained=True)
        raise
    finally:
        write_json(result / 'results.json', evidence)


if __name__ == '__main__':
    main()
