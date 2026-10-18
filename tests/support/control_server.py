"""Explicit per-run service launcher; no fixtures are created on import."""
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from .service_harness import ROOT, BIN, CACHE, port

def wait(predicate, timeout=7):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError("condition timed out")

def dead(pid):
    try:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].startswith("Z")
    except FileNotFoundError:
        return True

class Server:
    def __init__(self, run_dir, name, binary=BIN / "naki-live", launcher=False, environment=None):
        self.run_dir = run_dir
        self.name = name
        self.port = port()
        self.base = f"http://127.0.0.1:{self.port}"
        (CACHE / "wm-test-tmp").mkdir(parents=True, exist_ok=True)
        self.state = pathlib.Path(tempfile.mkdtemp(prefix="ctl-", dir=CACHE / "wm-test-tmp"))
        self.log = (self.run_dir / f"{name}.jsonl").open("w")
        self.error = (self.run_dir / f"{name}.stderr").open("w")
        args = ["--port", str(self.port), "--run-dir", str(self.run_dir / name), "--state-dir", str(self.state),
                "--worker-stop-timeout", "1", "--worker-timeout", "5", "--status-interval", "1"]
        command = [str(binary), *args]
        env = dict(os.environ)
        env.update(environment or {})
        if launcher:
            command = ["python3", str(ROOT / "scripts/service-launcher.py"), *args]
            env["NAKI_SERVICE_BINARY"] = str(binary)
        self.process = subprocess.Popen(command, stdout=self.log, stderr=self.error, env=env)
        wait(self.online)

    def online(self):
        assert self.process.poll() is None, (self.name, self.process.returncode)
        try:
            return self.api("/health")
        except OSError:
            return None

    def api(self, path="/v1/job", body=None, code=200):
        request = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=4)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = json.load(response)
            assert response.status == code, (path, response.status, data)
            return data

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=7)
        self.log.close()
        self.error.close()

    def silent(self, name):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            udp_port = sock.getsockname()[1]
        return self.api(body={"input": f"udp://127.0.0.1:{udp_port}",
                              "output": str(self.run_dir / f"{name}.ts")}, code=202)
