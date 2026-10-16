#!/usr/bin/env python3
"""Keep the container alive while a failed HTTP controller reconnects to its workers."""
import json
import signal
import subprocess
import sys
import time

stopping=False


def stop(signum, frame):
    global stopping
    stopping=True


def main():
    signal.signal(signal.SIGTERM,stop)
    signal.signal(signal.SIGINT,stop)
    delay=.25
    while not stopping:
        began=time.monotonic()
        child=subprocess.Popen(["/usr/local/bin/naki-live",*sys.argv[1:]])
        deadline=None
        while child.poll() is None:
            if stopping and deadline is None:
                child.send_signal(signal.SIGTERM)
                deadline=time.monotonic()+20
            if deadline is not None and time.monotonic()>=deadline:
                child.kill()
            time.sleep(.05)
        if stopping or child.returncode==0:return 0
        print(json.dumps({"event":"control_restarting","pid":child.pid,"exit_code":child.returncode,
                          "exit_signal":-child.returncode if child.returncode<0 else 0,
                          "retry_seconds":delay}),flush=True)
        until=time.monotonic()+delay
        while not stopping and time.monotonic()<until:time.sleep(.05)
        delay=.25 if time.monotonic()-began>30 else min(delay*2,10)
    return 0


if __name__=="__main__":sys.exit(main())
