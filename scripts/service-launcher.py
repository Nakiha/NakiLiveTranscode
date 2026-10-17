#!/usr/bin/env python3
"""Restart the controller and drain surviving workers on container termination."""
import json
import os
import signal
import subprocess
import sys
import time
import uuid

stop_time=None
instance_id=uuid.uuid4().hex


def stop(signum, frame):
    global stop_time
    if stop_time is None:stop_time=time.monotonic()


def emit(event,**fields):
    print(json.dumps(dict(event=event,role="launcher",instance_id=instance_id,**fields)),flush=True)


def budgets(args):
    worker=3
    for i,arg in enumerate(args):
        if arg=="--worker-stop-timeout":worker=int(args[i+1])
    hard=float(os.environ.get("NAKI_LAUNCHER_HARD_TIMEOUT","20"))
    grace=float(os.environ.get("NAKI_CONTAINER_STOP_GRACE","30"))
    if not 1<=worker<=60 or not worker+2<hard<grace:
        raise ValueError("require worker stop + 2 seconds < launcher hard timeout < container grace")
    return hard


def main():
    args=sys.argv[1:]
    try:hard=budgets(args)
    except (ValueError,IndexError) as error:
        print(str(error),file=sys.stderr);return 2
    binary=os.environ.get("NAKI_SERVICE_BINARY","/usr/local/bin/naki-live")
    signal.signal(signal.SIGTERM,stop)
    signal.signal(signal.SIGINT,stop)
    delay=.25
    phase="running";child=None;drain_started=False;notified=False;retry_at=0
    began=time.monotonic()
    while True:
        now=time.monotonic()
        if stop_time is not None:
            if phase!="draining":emit("control_draining",previous_state=phase);phase="draining"
            if now>=stop_time+hard:
                if child is not None and child.poll() is None:child.kill();child.wait()
                emit("control_drain_timeout",state="exited");return 1
            if child is None and not drain_started:
                try:child=subprocess.Popen([binary,*args,"--drain-only"])
                except OSError as error:emit("control_drain_start_failed",error=str(error));return 1
                drain_started=True;notified=True
            if child is not None and not notified:
                if child.poll() is None:child.send_signal(signal.SIGTERM)
                notified=True
        elif child is None and now>=retry_at:
            began=now
            try:child=subprocess.Popen([binary,*args]);phase="running"
            except OSError as error:
                emit("control_start_failed",error=str(error),retry_seconds=delay)
                phase="backoff";retry_at=now+delay;delay=min(delay*2,10)
        if child is not None:
            code=child.poll()
            if code is not None:
                pid=child.pid;child=None
                if stop_time is not None:
                    if code==0 or drain_started:
                        emit("control_drain_exited",exit_code=code,state="exited");return 0 if code==0 else 1
                    notified=False;continue
                if code==0:return 0
                emit("control_restarting",pid=pid,exit_code=code,exit_signal=-code if code<0 else 0,retry_seconds=delay)
                phase="backoff";retry_at=time.monotonic()+delay
                delay=.25 if time.monotonic()-began>30 else min(delay*2,10)
        time.sleep(.02)


if __name__=="__main__":sys.exit(main())
