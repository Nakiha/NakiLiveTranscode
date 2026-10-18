"""Bounded process checks shared by fault suites."""
import pathlib
import signal
import time

def wait_for(predicate, timeout=8):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        result=predicate()
        if result:return result
        time.sleep(.02)
    raise AssertionError("condition timed out")

def dead(pid):
    try:return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ",1)[1].startswith("Z")
    except FileNotFoundError:return True

def stop(server):
    if server.poll() is None:
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=5)
        assert server.returncode==0,server.returncode
