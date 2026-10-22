#!/usr/bin/env python3
"""Real worker acceptance, stale targets, recovery and launcher shutdown cut points."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import concurrent.futures
import json
import os
import pathlib
import select
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from support.event_logs import read_events

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = pathlib.Path(os.environ.get("NAKI_SERVICE_BIN", ROOT / "out/bin"))
RUN = None
CACHE = pathlib.Path(os.environ.get("NAKI_BUILD_ROOT", "/root/.cache/naki-live-transcode-n9"))
from support.control_server import Server, wait, dead
RESULTS = {}










def acceptance_exit():
    fixture = RUN / "gate-bin"
    fixture.mkdir()
    for name in ("naki-live", "naki-crash-collector"):
        (fixture / name).symlink_to(BIN / name)
    # /proc/self/exe resolves symlinks: copy only the small controller so its sibling is the gate.
    (fixture / "naki-live").unlink()
    import shutil
    shutil.copy2(BIN / "naki-live", fixture / "naki-live")
    gate = fixture / "naki-live-worker"
    marker = RUN / "first-worker"
    gate.write_text(f'#!/bin/bash\nif mkdir "{marker}" 2>/dev/null; then kill -STOP $$; fi\nexec "{BIN / "naki-live-worker"}" "$@"\n')
    gate.chmod(0o755)
    server = Server(RUN, "acceptance", fixture / "naki-live")
    try:
        worker = wait(lambda: next(iter(server.api("/health")["pool"]["workers"]), None))
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(server.api, "/v1/job", {"input": "udp://127.0.0.1:1", "output": str(RUN / "pending.ts")}, 503)
            wait(lambda: server.api()["supervisor"]["active"])
            time.sleep(.15)
            assert not future.done(), "202 must wait for worker ownership ACK"
            began = time.monotonic()
            os.kill(worker["pid"], signal.SIGKILL)
            failure = future.result(timeout=1.5)
            assert time.monotonic() - began < 1.5, failure
        wait(lambda: not server.api()["supervisor"]["active"])
        wait(lambda: server.api('/health')['admission']['open'])
        replacement = server.silent("replacement")
        assert replacement["supervisor"]["worker_pid"] != worker["pid"]
        wait(lambda: server.api('/health')['readiness']['ready'])
        server.api("/v1/job/stop", {"job_id": replacement["job_id"]}, 202)
        wait(lambda: server.api()["state"] == "stopped")
        RESULTS["acceptance-exit-pending-request-reaped-next-admission"] = "passed"
    finally:
        server.close()


def targets_and_recovery():
    server = Server(RUN, "targets")
    paused_spare = None
    try:
        first = server.silent("first")
        wait(lambda: server.api('/health')['readiness']['ready'])
        server.api("/v1/job/stop", {}, 400)
        server.api("/v1/job/stop", {"job_id": "stale"}, 409)
        assert server.api()["job_id"] == first["job_id"] and server.api()["state"] != "stopped"
        server.api("/v1/job/stop", {"job_id": first["job_id"]}, 202)
        wait(lambda: server.api()["state"] == "stopped")
        wait(lambda: server.api('/health')['admission']['open'])
        spec={"input":"udp://127.0.0.1:1","output":str(RUN/"second.ts")}
        second = server.api("/v1/jobs",{"operation_id":"create-second","spec":spec},202)
        replay=server.api("/v1/jobs",{"spec":dict(reversed(list(spec.items()))),"operation_id":"create-second"},202)
        assert replay["job_id"]==second["job_id"]
        server.api("/v1/jobs",{"operation_id":"create-second","spec":dict(spec,output=str(RUN/"conflict.ts"))},409)
        wait(lambda: server.api('/health')['readiness']['ready'])
        server.api("/v1/jobs",{"operation_id":"different-create","spec":spec},409)
        server.api("/v1/job/stop", {"job_id": first["job_id"]}, 409)
        pid = second["supervisor"]["worker_pid"]
        # Hold an unrelated standby unresolved across controller recovery.
        # Known create outcomes remain readable; this must not admit a new job.
        paused_spare = wait(lambda: next((w['pid'] for w in server.api('/health')['pool']['workers']
                                         if w['state'] == 'ready' and w['pid'] != pid), None))
        os.kill(paused_spare, signal.SIGSTOP)
        # Exercise the worker's second target check through its authenticated private endpoint.
        server.process.kill()
        server.process.wait(timeout=3)
        def connect():
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            try:
                connection.connect(str(server.state / f"w-{pid}.sock"))
                return connection
            except OSError:
                connection.close()
                return None
        with wait(connect) as connection:
            connection.settimeout(2)
            snapshot = json.loads(connection.recv(65536))
            connection.send(json.dumps(dict(ipc_version=2, ipc_kind="stop", instance_id="0" * 32,
                                           worker_instance_id=snapshot["instance_id"], job_id=first["job_id"])).encode())
            answer = wait(lambda: (value if (value := json.loads(connection.recv(65536)))["ipc_kind"] == "control_rejected" else None))
            assert answer["error_code"] == "target_mismatch" and not dead(pid)
        # Reconnect a fresh controller to exactly the same state and preserve the assignment.
        command = [str(BIN / "naki-live"), "--port", str(server.port), "--run-dir", str(RUN / "targets"),
                   "--state-dir", str(server.state), "--worker-stop-timeout", "1"]
        server.process = subprocess.Popen(command, stdout=server.log, stderr=server.error)
        wait(server.online)
        recovered = wait(lambda: (state if (state := server.api()).get("job_id") == second["job_id"] and state["supervisor"]["recovered"] else None))
        assert recovered["supervisor"]["worker_pid"] == pid
        health = server.api('/health')
        assert health['pool']['recovering'] > 0 and not health['readiness']['ready'], health
        replay=server.api("/v1/jobs",{"operation_id":"create-second","spec":spec},202)
        assert replay["job_id"]==second["job_id"]
        server.api('/v1/jobs', {'operation_id': 'must-not-create-recovering', 'spec': spec}, 503)
        server.api('/v1/jobs', {'operation_id': 'create-second', 'spec': dict(spec, output=str(RUN/'conflict-recovering.ts'))}, 409)
        assert server.api()['job_id'] == second['job_id']
        os.kill(paused_spare, signal.SIGCONT)
        paused_spare = None
        wait(lambda: server.api('/health')['readiness']['ready'])
        stop_path=f'/v1/jobs/{second["job_id"]}/stop'
        accepted_stop=server.api(stop_path,{"operation_id":"stop-second"},202)
        assert accepted_stop["kind"]=="stop" and accepted_stop["outcome_known"]
        wait(lambda: server.api()["state"] == "stopped")
        wait(lambda: server.api('/health')['admission']['open'])
        third=server.silent("third")
        wait(lambda: server.api('/health')['readiness']['ready'])
        replay=server.api("/v1/jobs",{"operation_id":"create-second","spec":spec},202)
        assert replay["job_id"]==second["job_id"] and server.api()["job_id"]==third["job_id"]
        replay_stop=server.api(stop_path,{"operation_id":"stop-second"},202)
        assert replay_stop["state"]=="stopped" and server.api()["job_id"]==third["job_id"]
        server.api(stop_path,{"operation_id":"late-new-stop"},409)
        retained_stop=server.api(f'/v1/jobs/{second["job_id"]}/operations/stop-second')
        assert retained_stop["state"]=="stopped" and retained_stop["outcome_known"]
        previous=server.api(f'/v1/jobs/{second["job_id"]}')
        assert previous["state"]=="stopped"
        operation=server.api(f'/v1/jobs/{second["job_id"]}/operations/create-second')
        assert operation["state"]=="accepted" and operation["outcome_known"]
        server.api("/v1/job/stop",{"job_id":third["job_id"]},202)
        wait(lambda:server.api()["state"]=="stopped")
        RESULTS["stale-http-and-private-ipc-targets-real-recovery"] = "passed"
    finally:
        if paused_spare is not None:
            try:
                os.kill(paused_spare, signal.SIGCONT)
            except ProcessLookupError:
                pass
        server.close()


def transient_recovery():
    sample=RUN/"recovery-input.mp4"
    subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=160x90:rate=25",
                    "-frames:v","300","-c:v","libx264","-threads","2",str(sample)],check=True,timeout=20)
    server=Server(RUN, "transient")
    try:
        output=RUN/"recovery-output.ts"
        assigned=server.api(body={"input":str(sample),"output":str(output),"pace":True},code=202)
        pid=assigned["supervisor"]["worker_pid"]
        wait(lambda:server.api().get("packets_out",0)>5)
        before=output.stat().st_size
        server.process.kill();server.process.wait(timeout=3)
        command=[str(BIN/"naki-live"),"--port",str(server.port),"--run-dir",str(RUN/"transient"),
                 "--state-dir",str(server.state),"--worker-timeout","1","--worker-stop-timeout","1"]
        assert (BIN/"libnaki-control-connect-fault.so").is_file()
        env=dict(os.environ,LD_PRELOAD=str(BIN/"libnaki-control-connect-fault.so"),NAKI_TEST_CONNECT_FAILURE_MS="3500")
        server.process=subprocess.Popen(command,stdout=server.log,stderr=server.error,env=env)
        wait(server.online)
        recovering=server.api("/health")
        assert recovering["pool"]["recovering"]>0,recovering
        assert recovering['liveness']['alive'] and not recovering['readiness']['ready'] and not recovering['admission']['open'],recovering
        assert not server.api('/health/ready',code=503)['readiness']['ready']
        assert server.api('/health/live')['liveness']['alive']
        server.api(body={"input":str(sample),"output":str(RUN/"must-not-start.ts")},code=503)
        time.sleep(2)
        assert not dead(pid) and output.stat().st_size>before
        assert server.api("/health")["pool"]["recovering"]>0
        recovered=wait(lambda:(value if (value:=server.api()).get("job_id")==assigned["job_id"] and value["supervisor"]["recovered"] else None))
        assert recovered["supervisor"]["worker_pid"]==pid
        wait(lambda: server.api('/health')['readiness']['ready'])
        assert server.api('/health/ready')['readiness']['ready']
        server.api("/v1/job/stop",{"job_id":assigned["job_id"]},202)
        wait(lambda:server.api()["state"]=="stopped")
        RESULTS["transient-eagain-keeps-admission-closed-and-media-running"]="passed"
    finally:server.close()


def launcher_backoff():
    server = Server(RUN, "launcher", launcher=True)
    try:
        assigned = server.silent("launcher")
        health = server.api("/health")
        controller = health["supervisor_pid"]
        workers = [entry["pid"] for entry in health["pool"]["workers"]]
        os.kill(controller, signal.SIGKILL)
        path = RUN / "launcher.jsonl"
        wait(lambda: '"event": "control_restarting"' in path.read_text())
        began = time.monotonic()
        server.process.terminate()
        server.process.wait(timeout=7)
        assert server.process.returncode == 0, path.read_text()[-4000:]
        assert time.monotonic() - began < 6
        assert all(dead(pid) for pid in workers), workers
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        assert any(row.get("event") == "control_draining" and row["previous_state"] == "backoff" for row in rows)
        assert any(row.get("event") == "worker_recovered" and row.get("worker_pid") == assigned["supervisor"]["worker_pid"] for row in read_events(RUN/"launcher/events"))
        RESULTS["sigterm-in-launcher-backoff-drains-surviving-real-worker"] = "passed"
    finally:
        server.close()


def incompatible_worker(result_version=False):
    name='incompatible-result-version' if result_version else 'incompatible'
    marker=RUN/(name+'-marker')
    environment=dict(LD_PRELOAD=str(BIN/"libnaki-control-connect-fault.so"))
    environment['NAKI_TEST_OLD_RESULT_VERSION_MARKER' if result_version else 'NAKI_TEST_OLD_PROTOCOL_MARKER']=str(marker)
    server=Server(RUN, name,environment=environment)
    try:
        output=RUN/(name+'-output.ts')
        assigned=server.api(body={"input":str(RUN/"recovery-input.mp4"),"output":str(output),"pace":True},code=202)
        pid=assigned["supervisor"]["worker_pid"]
        wait(lambda:server.api().get("packets_out",0)>5)
        before=output.stat().st_size
        server.process.kill();server.process.wait(timeout=3)
        marker.touch()
        command=[str(BIN/"naki-live"),"--port",str(server.port),"--run-dir",str(RUN/name),
                 "--state-dir",str(server.state),"--worker-timeout","1","--worker-stop-timeout","1"]
        server.process=subprocess.Popen(command,stdout=server.log,stderr=server.error)
        wait(server.online)
        def classified():
            h=server.api("/health")
            return h if any(w["state"]=="incompatible" for w in h["pool"]["workers"]) else None
        health=wait(classified)
        assert health["pool"]["recovering"]>0
        assert health['liveness']['alive'] and not health['readiness']['ready'] and not health['admission']['open'],health
        assert not server.api('/health/ready',code=503)['readiness']['ready']
        server.api(body={"input":str(RUN/"recovery-input.mp4"),"output":str(RUN/"no-incompatible-admission.ts")},code=503)
        time.sleep(1.5)
        assert not dead(pid) and output.stat().st_size>before
        assert server.api("/health")["pool"]["recovering"]>0
        if result_version:assert 'capabilit' in health['pool']['error'].lower(),health
        RESULTS['live-unversioned-result-worker-visible-admission-closed-media-continues' if result_version else 'live-incompatible-worker-visible-admission-closed-media-continues']="passed"
    finally:
        marker.unlink(missing_ok=True)
        server.close()


def lost_acceptance():
    marker=RUN/"drop-acceptance";marker.touch()
    environment=dict(LD_PRELOAD=str(BIN/"libnaki-control-connect-fault.so"),NAKI_TEST_DROP_ACCEPTANCE_MARKER=str(marker))
    server=Server(RUN, "lost-acceptance",environment=environment)
    try:
        output=RUN/"lost-acceptance.ts"
        spec={"input":str(RUN/"recovery-input.mp4"),"output":str(output),"pace":True}
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            pending=pool.submit(server.api,"/v1/jobs",{"operation_id":"lost-ack-create","spec":spec},202)
            wait(lambda:output.exists() and output.stat().st_size>0)
            assert not pending.done(),"HTTP must not claim accepted while ACK/snapshot are withheld"
            workers=server.api("/health")["pool"]["workers"]
            server.process.kill();server.process.wait(timeout=3)
            try:pending.result(timeout=3)
            except (OSError,ConnectionError):pass
            else:raise AssertionError("controller death must close the pending HTTP attempt")
        marker.unlink()
        server.process=subprocess.Popen([str(BIN/"naki-live"),"--port",str(server.port),"--run-dir",str(RUN/"lost-acceptance"),
                                         "--state-dir",str(server.state),"--worker-stop-timeout","1"],stdout=server.log,stderr=server.error)
        wait(server.online)
        recovered=wait(lambda:(s if (s:=server.api()).get("creation_operation_id")=="lost-ack-create" else None))
        original=recovered["job_id"]
        retry=server.api("/v1/jobs",{"operation_id":"lost-ack-create","spec":spec},202)
        assert retry["job_id"]==original and recovered["supervisor"]["worker_pid"] in [w["pid"] for w in workers]
        wait(lambda: server.api('/health')['readiness']['ready'])
        server.api("/v1/job/stop",{"job_id":original},202)
        wait(lambda:server.api()["state"]=="stopped")
        RESULTS["worker-owned-before-ack-http-loss-retry-recovers-original-job"]="passed"
    finally:
        marker.unlink(missing_ok=True);server.close()


if __name__ == "__main__":
    RUN = pathlib.Path(tempfile.mkdtemp(prefix="control-lifecycle-", dir=ROOT / "out"))
    try:
        acceptance_exit()
        targets_and_recovery()
        transient_recovery()
        incompatible_worker()
        incompatible_worker(True)
        lost_acceptance()
        launcher_backoff()
        (RUN / "results.json").write_text(json.dumps(RESULTS, indent=2) + "\n")
        print(json.dumps(dict(run=str(RUN), results=RESULTS), indent=2))
    finally:
        print(f"Evidence: {RUN}", flush=True)
