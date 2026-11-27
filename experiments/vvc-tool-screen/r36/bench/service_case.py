"""One isolated service job, including loaded-module and actual mux checks."""
from __future__ import annotations
import hashlib
import json
import math
import os
import re
import signal
import socket
import statistics
import subprocess as sp
import sys
import tempfile
import time
import urllib.error
import urllib.request
from fractions import Fraction
from pathlib import Path

sys.dont_write_bytecode = True

import threading
CLK_TCK = os.sysconf('SC_CLK_TCK')
port_lock = threading.Lock()
ports_used = set()
direct_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def child_ticks(pid):
    stat=Path('/proc/%d/stat' % pid).read_text()
    fields=stat[stat.rfind(')')+2:].split()
    return int(fields[13])+int(fields[14])

def setup(bundle):
    global BUNDLE, CASES, MEDIA_DIR, FFMPEG_LIB, ENCODER_LIB, ARMS, FFPROBE
    BUNDLE = bundle.resolve()
    MEDIA_DIR = BUNDLE/'bench'
    FFPROBE = MEDIA_DIR/'ffprobe'
    FFMPEG_LIB = ENCODER_LIB = BUNDLE/'lib'
    CASES = json.loads((MEDIA_DIR/'cases.json').read_text())
    ARMS = {name: dict(bin_dir=BUNDLE/'bin', vvenc_dir=lib, ffmpeg_dir=FFMPEG_LIB)
            for name,lib in (('r35',MEDIA_DIR/'r35-lib'),('r36',ENCODER_LIB))}

def sha(path: Path) -> str:
    with path.open('rb') as f:
        h = hashlib.sha256()
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
        return h.hexdigest()


def save(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2) + '\n')
    tmp.replace(path)


def percentile(values, frac):
    s = sorted(values)
    return s[min(len(s) - 1, math.ceil(frac * len(s)) - 1)]


def free_port():
    with port_lock:
        while True:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.bind(('127.0.0.1', 0))
                port = s.getsockname()[1]
            if port not in ports_used:
                ports_used.add(port)
                return port


def http_json(port, path='/v1/job', data=None, expected_code=200):
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(
        f'http://127.0.0.1:{port}{path}',
        data=body,
        headers={'Content-Type': 'application/json'},
    )
    try:
        resp = direct_http.open(req, timeout=5)
    except urllib.error.HTTPError as err:
        resp = err
    with resp:
        payload = json.load(resp)
        assert resp.status == expected_code, (path, resp.status, payload)
        return payload


def read_proc_children_cpu_ticks(pid: int) -> int:
    stat = Path(f'/proc/{pid}/stat').read_text()
    rparen = stat.rfind(')')
    fields = stat[rparen + 2:].split()
    utime = int(fields[11])
    stime = int(fields[12])
    cutime = int(fields[13])
    cstime = int(fields[14])
    return utime + stime + cutime + cstime


def find_worker_and_verify_maps(assigned_pid: int, worker_exe: Path, run_dir: str, required_paths: list[Path]):
    resolved_exe = worker_exe.resolve()
    resolved_libs = [str(p.resolve()) for p in required_paths]
    for pid_dir in [Path('/proc')/str(assigned_pid)]:
        try:
            if (pid_dir / 'exe').resolve() != resolved_exe:
                continue
            cmdline = (pid_dir / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
            if run_dir not in cmdline:
                continue
            maps = (pid_dir / 'maps').read_text()
            if all(lib in maps for lib in resolved_libs):
                return int(pid_dir.name)
        except (OSError, RuntimeError):
            continue
    return None


def probe_packets(mp4_path: Path):
    probe_env = dict(os.environ, LD_LIBRARY_PATH=f"{ENCODER_LIB}:{FFMPEG_LIB}")
    out = sp.check_output(
        [str(FFPROBE), '-v', 'error', '-select_streams', 'v:0', '-show_packets',
         '-show_entries', 'stream=codec_name,time_base:packet=pts,dts,size', '-of', 'json', str(mp4_path)],
        env=probe_env,
        text=True,
    )
    data = json.loads(out)
    tb = Fraction(data['streams'][0]['time_base'])
    pkts = [(int(p['pts']) * tb, int(p['dts']) * tb, int(p['size'])) for p in data['packets']]
    return data['streams'][0]['codec_name'], pkts


def run_one(run_root: Path, arm: str, tier: str, clip: str, slots: int, threads: int, lane: str, paced: bool, rep: int):
    spec = ARMS[arm]
    import ctypes, mmap
    images = sorted({p.resolve() for p in [spec['bin_dir']/'naki-live',spec['bin_dir']/'naki-live-worker',spec['vvenc_dir']/'libvvenc.so',*spec['ffmpeg_dir'].glob('libav*.so'),*spec['ffmpeg_dir'].glob('libsw*.so')]})
    pages = {}
    for image in images:
        assert BUNDLE in image.parents
        with image.open('rb') as f:
            while f.read(2**20): pass
            with mmap.mmap(f.fileno(),0,access=mmap.ACCESS_COPY) as mapping:
                n=(len(mapping)+mmap.PAGESIZE-1)//mmap.PAGESIZE
                vec=(ctypes.c_ubyte*n)()
                addr=ctypes.addressof(ctypes.c_char.from_buffer(mapping))
                assert ctypes.CDLL(None).mincore(ctypes.c_void_p(addr),ctypes.c_size_t(len(mapping)),vec)==0
                pages[image.name]=[sum(v&1 for v in vec),n]

    bin_dir = spec['bin_dir']
    vvenc_so = (spec['vvenc_dir'] / 'libvvenc.so').resolve()
    avcodec_so = (spec['ffmpeg_dir'] / 'libavcodec.so.63').resolve()
    worker_exe = bin_dir / 'naki-live-worker'
    server_exe = bin_dir / 'naki-live'

    env = dict(os.environ, LD_LIBRARY_PATH=f"{spec['vvenc_dir']}:{spec['ffmpeg_dir']}")
    case = CASES[tier][clip]
    n_frames = int(case['frames'])
    src_ts = MEDIA_DIR / f'{clip}.ts'
    target = run_root / 'output.mp4'

    run_root.mkdir(parents=True, exist_ok=True)
    target.unlink(missing_ok=True)
    cfg = dict(
        input=str(src_ts),
        output=str(target),
        output_format='mp4',
        encoder='libvvenc',
        encoder_profile='r28_' + tier,
        rate_control='constrained_crf',
        crf=case['qp'],
        maxrate=case['controller_cap'],
        rate_burst_ms=case['controller_burst'],
        encoder_threads=threads,
        encoder_parallel_frames=slots,
        pace=paced,
        frame_log=True,
    )
    save(run_root / 'request.json', cfg)
    port = free_port()

    worker_pid = None
    cpu_ticks_total = 0
    samples = []
    with tempfile.TemporaryDirectory(prefix='s', dir='/tmp') as sock_dir, (run_root / 'server.log').open('w') as log:
        proc = sp.Popen(
            ['taskset', '-c', lane, str(server_exe), '--port', str(port), '--run-dir', sock_dir, '--state-dir', sock_dir+'/state', '--spare-workers', '1', '--status-interval', '1'],
            stdout=log,
            stderr=sp.STDOUT,
            env=env,
        )
        try:
            for _ in range(200):
                try:
                    health=http_json(port, '/health')
                    if health.get('vvenc_enabled') and health['pool']['ready']>=1:
                        break
                    time.sleep(0.025)
                except OSError:
                    if proc.poll() is not None:
                        raise RuntimeError((run_root / 'server.log').read_text()[-800:])
                    time.sleep(0.03)
            else:
                raise RuntimeError('Server health timeout')

            before_reaped = child_ticks(proc.pid)
            accepted=http_json(port, '/v1/job', cfg, expected_code=202)
            assigned_pid=accepted['supervisor']['worker_pid']
            for _ in range(150):
                worker_pid = find_worker_and_verify_maps(assigned_pid,worker_exe, sock_dir, [vvenc_so, avcodec_so])
                if worker_pid is not None:
                    break
                time.sleep(0.015)
            assert worker_pid is not None, f'Failed to verify loaded libraries in worker for {arm}'

            deadline = time.monotonic() + 180
            state = None
            while time.monotonic() < deadline:
                try:
                    p = Path(f'/proc/{worker_pid}')
                    status = (p/'status').read_text()
                    rss = re.search(r'^VmRSS:\s+(\d+)', status, re.M)
                    peak = re.search(r'^VmHWM:\s+(\d+)', status, re.M)
                    if rss and peak:
                        samples.append((time.monotonic_ns()//1000, int(rss[1]), int(peak[1]), read_proc_children_cpu_ticks(worker_pid)))
                except (OSError, ValueError):
                    pass
                state = http_json(port, '/v1/job')
                if state['state'] in ('completed', 'completed_partial', 'stopped', 'failed'):
                    assert state['state'] == 'completed', state
                    break
                time.sleep(0.03)
            assert state is not None and state['state'] == 'completed', 'Job timed out'
            # Allow a moment for supervisor to reap finished worker into server cutime+cstime
            time.sleep(0.05)
            try:
                reaped = max(0, child_ticks(proc.pid) - before_reaped)
                try:
                    last_worker = read_proc_children_cpu_ticks(worker_pid)
                except OSError:
                    last_worker = 0
                sampled = max((x[3] for x in samples), default=0)
                cpu_ticks_total = max(sampled, last_worker, reaped)
                cpu_source = 'isolated server child CPU after reap' if reaped >= max(sampled, last_worker) else 'worker procfs sample'
            except OSError:
                cpu_ticks_total = max((x[3] for x in samples),default=0)
                cpu_source = "worker procfs sample"

            save(run_root / 'state.json', state)
            assert state['startup']['prewarmed']
            assert state['packets_out'] == state['frames_filtered'] == n_frames, state
            assert state['frames_dropped_overload'] == 0 and state['frames_dropped_rate'] == 0, state

            log_prefix = Path(state['log_path'])
            events = [
                json.loads(line)
                for f in sorted(log_prefix.parent.glob(log_prefix.name + '.*.jsonl'))
                for line in f.read_text().splitlines()
                if line
            ]
            event_files = sorted((Path(sock_dir)/'events').glob('worker-*.events.*.jsonl*'))
            worker_events = [json.loads(line) for f in event_files for line in f.read_text().splitlines() if line]
            ready = next(e for e in worker_events if e.get('event')=='pipeline_ready' and e.get('job_id')==state['job_id'])
            first_outputs = [e for e in worker_events if e.get('event')=='first_frame' and e.get('job_id')==state['job_id']]
            save(run_root/'first_frame.json', first_outputs)
            save(run_root/'pipeline_ready.json',ready)
            save(run_root/'worker_samples.json',samples)
            records = [e for e in events if e.get('event') == 'frame']
            assert len(records) == len({e['input_sequence'] for e in records}) == n_frames
            for e in records:
                exact = (e['mux_flush_done_monotonic_us'] - e['packet_ingress_monotonic_us']) / 1000.0
                assert abs(exact - e['ingress_to_mux_ms']) < 0.002, (exact, e['ingress_to_mux_ms'])
            save(run_root / 'frames.json', records)
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=20)
            except sp.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
                raise

    codec, pkts = probe_packets(target)
    assert codec == 'vvc' and len(pkts) == n_frames and len({p for p, _, _ in pkts}) == n_frames
    assert all(a[1] < b[1] for a, b in zip(pkts, pkts[1:]))

    bits_by_mux = [8 * r['encoded_bytes'] for r in sorted(records, key=lambda x: x['mux_flush_done_monotonic_us'])]
    peak5 = max(sum(bits_by_mux[i:i + 150]) / 5.0 for i in range(len(bits_by_mux) - 149))
    assert peak5 <= cfg['maxrate'], (peak5, cfg['maxrate'])
    actual_kbps = sum(bits_by_mux) * 0.03 / len(bits_by_mux)

    ts_mux = sorted(r['mux_flush_done_monotonic_us'] for r in records)
    gaps = [(b - a) / 1000.0 for a, b in zip(ts_mux, ts_mux[1:])]

    first_ingress = min(r['packet_ingress_monotonic_us'] for r in records)
    last_mux = max(r['mux_flush_done_monotonic_us'] for r in records)
    pipeline_wall_ms = (last_mux - first_ingress) / 1000.0
    job_elapsed_ms = state['elapsed_ms']
    cpu_sec = max(0.0, cpu_ticks_total / CLK_TCK)

    phases = {}
    for field in ('ingress_to_mux_ms', 'scheduled_to_mux_ms', 'encode_wait_ms', 'encode_submit_ms', 'encode_ms', 'setup_or_dispatch_ms', 'mux_ms'):
        if field not in records[0]:
            continue
        vals = [r[field] for r in records]
        phases[field] = {
            'mean': statistics.fmean(vals),
            'p50': percentile(vals, 0.50),
            'p95': percentile(vals, 0.95),
            'p99': percentile(vals, 0.99),
            'max': max(vals),
        }

    steady = [x[1] for x in samples if ts_mux[59] <= x[0] <= ts_mux[-11]]
    assert steady and samples
    completion = sorted(records,key=lambda r:r['mux_flush_done_monotonic_us'])
    i2m = [r['ingress_to_mux_ms'] for r in completion[60:]]
    result = {
        'startup_prewarmed':state['startup']['prewarmed'], 'assigned_worker_pid':assigned_pid,
        'encoder_open_ms': state['startup']['encoder_open_done_ms']-state['startup']['encoder_open_started_ms'],
        'gap_1_2_ms': gaps[0],
        'cache':'warm', 'resident_file_pages_before_server':pages,
        'open_stages': {k:v for k,v in ready.items() if k.startswith('vvenc_open_')},
        'width':ready['output_width'], 'height':ready['output_height'],
        'peak_worker_rss_kib':max(x[2] for x in samples),
        'steady_worker_rss_kib':statistics.median(steady),
        'worker_cpu_pct':100*max(x[3] for x in samples)/CLK_TCK/(job_elapsed_ms/1000),
        'steady_i2m_ms':dict(mean=statistics.fmean(i2m),p50=percentile(i2m,.50),p95=percentile(i2m,.95),p99=percentile(i2m,.99)),
        'arm': arm,
        'tier': tier,
        'clip': clip,
        'slots': slots,
        'threads': threads,
        'lane': lane,
        'paced': paced,
        'rep': rep,
        'frames': n_frames,
        'cap_bps': cfg['maxrate'],
        'actual_kbps': actual_kbps,
        'peak5_bps': peak5,
        'dropped': state['frames_dropped_overload'] + state['frames_dropped_rate'],
        'future_max': max(r['encoder_future_frames'] for r in records),
        'http_to_first_video_ms': state['startup']['request_to_first_video_mux_ms'],
        'job_elapsed_ms': job_elapsed_ms,
        'job_fps': n_frames / (job_elapsed_ms / 1000.0),
        'pipeline_wall_ms': pipeline_wall_ms,
        'pipeline_fps': n_frames / (pipeline_wall_ms / 1000.0),
        'worker_cpu_source':cpu_source,
        'worker_cpu_ms_per_frame': (cpu_sec * 1000.0) / n_frames,
        'worst_write_gap_ms': max(gaps),
        'p99_write_gap_ms': percentile(gaps, 0.99),
        'phases': phases,
        'sdk_sha256': sha(vvenc_so),
        'avcodec_sha256': sha(avcodec_so),
        'worker_sha256': sha(worker_exe),
        'mp4_path': str(target),
        'mp4_sha256': sha(target),
        'source_sha256': sha(src_ts),
        'frames_json_sha256': sha(run_root / 'frames.json'),
    }
    assert result['future_max'] <= 10
    save(run_root / 'result.json', result)
    print(
        f"[{arm:14s} paced={int(paced)} {tier}-{clip} P{slots}/T{threads} rep{rep}] "
        f"kbps={actual_kbps:.1f} i2m_p50/p95/p99={phases['ingress_to_mux_ms']['p50']:.1f}/"
        f"{phases['ingress_to_mux_ms']['p95']:.1f}/{phases['ingress_to_mux_ms']['p99']:.1f}ms "
        f"wait_p50/p95={phases['encode_wait_ms']['p50']:.1f}/{phases['encode_wait_ms']['p95']:.1f}ms "
        f"job_fps={result['job_fps']:.2f} pipe_fps={result['pipeline_fps']:.2f} cpu={result['worker_cpu_ms_per_frame']:.1f}ms/f",
        flush=True,
    )
    return result
