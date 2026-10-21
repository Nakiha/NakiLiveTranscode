"""HTTP/trace helpers and per-suite state; imports perform no I/O."""
import json
import math
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = pathlib.Path(os.environ.get('NAKI_SERVICE_BIN', ROOT / 'out/bin'))
CACHE = pathlib.Path(os.environ.get('NAKI_BUILD_ROOT', pathlib.Path(os.environ.get(
    'XDG_CACHE_HOME', pathlib.Path.home() / '.cache')) / 'naki-live-transcode-n9'))
PROBE = CACHE / 'install/bin/ffprobe'

def new_run(prefix):
    (ROOT / 'out').mkdir(exist_ok=True)
    return pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=ROOT / 'out'))

def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def probe(path):
    result = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                             "-count_frames", "-show_entries", "stream=width,height,codec_name,nb_read_frames",
                             "-of", "json", str(path)], check=True, capture_output=True, text=True)
    return json.loads(result.stdout)["streams"][0]


def check_duration(path, expected):
    result = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                             "-of", "json", str(path)], check=True, capture_output=True, text=True)
    actual = float(json.loads(result.stdout)["format"]["duration"])
    assert abs(actual - expected) < 0.05, (path, actual, expected)


def check_trace(state, count, source=None, reordered=False):
    prefix=pathlib.Path(state["log_path"])
    deadline=time.monotonic()+5
    lines=[]
    while time.monotonic()<deadline:
        files=sorted(prefix.parent.glob(prefix.name+".*.jsonl"))
        if not list(prefix.parent.glob(prefix.name+".*.active")) and files:
            lines=[line for path in files for line in path.read_text().splitlines()]
            if len(lines)==count:break
        time.sleep(0.02)
    events = [json.loads(line) for line in lines]
    frames = [event for event in events if event["event"] == "frame"]
    assert len(frames) == count, (count, len(frames))
    assert len({row["input_sequence"] for row in frames}) == count
    if not reordered:
        assert [row["pts_us"] for row in frames] == sorted(row["pts_us"] for row in frames)
    assert state["frames_decoded"] == state["frames_filtered"] == state["packets_out"] == count
    assert state["auto"]["maximum"] == 12 and state["auto"]["minimum"] == 2
    assert state["logging"]["dropped_frames"]==0
    assert state["frame_types"]["first_i"]["count"]==1
    assert state["frame_types"]["i_frame"]["count"]>=1
    assert state["i_frame_stages"]["ingress_to_mux"]["count"]==state["frame_types"]["i_frame"]["count"]
    assert sum(state["frame_types"][name]["count"] for name in ("i_frame","p_frame","b_frame"))==count
    values=sorted(row["ingress_to_mux_ms"] for row in frames)
    metric=state["ingress_to_mux"]
    assert abs(metric["mean_ms"]-sum(values)/len(values))<1e-6
    assert abs(metric["max_ms"]-max(values))<1e-6
    assert metric["slow_count"]==sum(value>=state["slow_frame_ms"] for value in values)
    for percent in (50,95,99):
        true_value=values[math.ceil(count*percent/100)-1]
        upper=metric[f"p{percent}_upper_ms"]
        if true_value>=16000:assert upper is None
        else:
            width=1 if true_value<200 else 20 if true_value<1000 else 1000
            assert true_value<=upper+1e-9 and upper-true_value<=width+1e-9
    for row in frames:
        for key in ("decode_ms", "filter_ms", "encode_ms", "mux_ms", "input_queue_ms", "ingress_to_mux_ms"):
            assert math.isfinite(row[key]) and row[key] >= 0, (key, row)
        parts = sum(row[key] for key in ("input_queue_ms", "decode_ms", "setup_or_dispatch_ms", "filter_ms", "encode_ms", "mux_ms"))
        assert abs(parts - row["ingress_to_mux_ms"]) < 3, (parts, row)
    if source:
        result = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
                                 "-show_entries", "packet=pts_time", "-of", "json", str(source)],
                                check=True, capture_output=True, text=True)
        packets = json.loads(result.stdout)["packets"]
        for row in frames:
            expected = float(packets[row["input_sequence"]]["pts_time"]) * 1_000_000
            assert abs(row["pts_us"] - expected) < 15, (row, expected)
        result=subprocess.run(["ffprobe","-v","error","-select_streams","v:0","-show_frames",
                               "-show_entries","frame=pict_type,key_frame,best_effort_timestamp_time","-of","json",str(source)],
                              check=True,capture_output=True,text=True)
        reference={round(float(frame["best_effort_timestamp_time"])*1e6):frame for frame in json.loads(result.stdout)["frames"]}
        for row in frames:
            key=round(float(packets[row["input_sequence"]]["pts_time"])*1e6)
            assert row["decoded_picture_type"]==reference[key]["pict_type"]
            assert row["decoded_key"]==reference[key]["key_frame"]
    return frames

class ServiceHarness:
    """A private endpoint and evidence directory, acquired only when first used."""
    ROOT, BIN = ROOT, BIN
    port = staticmethod(port)
    probe = staticmethod(probe)
    check_duration = staticmethod(check_duration)
    check_trace = staticmethod(check_trace)

    def __init__(self, prefix='service-test-'):
        self.prefix = prefix
        self._run = None
        self._port = None

    @property
    def RUN(self):
        if self._run is None:
            self._run = new_run(self.prefix)
        return self._run

    @property
    def PORT(self):
        if self._port is None:
            self._port = port()
        return self._port

    @property
    def BASE(self):
        return f'http://127.0.0.1:{self.PORT}'

    def http(self, path="/v1/job", data=None, code=200, raw=None, method=None):
        if path in ("/v1/job/stop","/v1/job/watermark") and isinstance(data,dict) and "job_id" not in data:
            data=dict(data,job_id=self.http()["job_id"])
        body = raw if raw is not None else json.dumps(data).encode() if data is not None else None
        req = urllib.request.Request(self.BASE + path, data=body, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(req, timeout=3)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            payload = json.load(response)
            assert response.status == code, (path, response.status, payload)
            return payload

    def finished(self, expected="completed", timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.http()
            if state["state"] in ("completed", "completed_partial", "stopped", "failed"):
                assert state["state"] == expected, state
                return state
            time.sleep(0.04)
        raise AssertionError(("job timed out", self.http()))

    def start(self, config):
        return self.http(data=config, code=202)
