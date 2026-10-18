#!/usr/bin/env python3
"""Real faults, bounded external unwinding, quota/timeout failures, worker and HTTP recovery."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from support.service_harness import ServiceHarness
service_client = ServiceHarness(prefix="crash-")
ROOT = service_client.ROOT
BIN = service_client.BIN
http = service_client.http
start = service_client.start
finished = service_client.finished
port = service_client.port
probe = service_client.probe
from support.processes import wait_for, dead, stop


def unique_object(pairs):
    assert len(pairs)==len({key for key,value in pairs}),pairs
    return dict(pairs)


def report(directory,pid):
    files=list(directory.glob(f"crash-{pid}-*.json"))
    return json.loads(files[0].read_text(),object_pairs_hook=unique_object) if files else None


def fixture(directory,mode="segv",extra=None):
    directory.mkdir(mode=0o700,exist_ok=True)
    env=dict(os.environ,NAKI_CRASH_DIR=str(directory),NAKI_CRASH_TIMEOUT_MS="2000")
    if extra:env.update(extra)
    began=time.monotonic()
    child=subprocess.Popen([str(BIN/"naki-crash-fixture"),mode],env=env,cwd=RUN,
                           stdout=subprocess.PIPE,stderr=(RUN/"fixtures.stderr").open("a"),text=True)
    line=child.stdout.readline()
    assert line,(mode,child.poll())
    pid,collector=map(int,line.split())
    return child,pid,collector,began


def crash_fixture(directory,mode="segv",extra=None):
    child,pid,collector,began=fixture(directory,mode,extra)
    try:
        child.wait(timeout=4)
        assert child.returncode==-signal.SIGSEGV,(mode,child.returncode)
        result=wait_for(lambda:report(directory,pid),timeout=4)
        assert result["pid"]==pid and result["signal"]==signal.SIGSEGV and not result["full_core"]
        return result,time.monotonic()-began
    finally:
        if child.poll() is None:child.kill();child.wait()
        child.stdout.close()


def server(directory):
    logs=(RUN/"server.jsonl").open("a");errors=(RUN/"server.stderr").open("a")
    child=subprocess.Popen([str(BIN/"naki-live"),"--port",str(PORT),"--run-dir",str(RUN/"service"),
                            "--crash-dir",str(directory),"--status-interval","1"],stdout=logs,stderr=errors,cwd=ROOT)
    logs.close();errors.close()
    def online():
        assert child.poll() is None,child.returncode
        try:return http("/health")
        except OSError:return False
    wait_for(online)
    return child


def run():
    global RUN, PORT
    RUN, PORT = service_client.RUN, service_client.PORT
    with tempfile.TemporaryDirectory(prefix="naki-crash-test-") as private:
        directory=pathlib.Path(private)
        normal=directory/"normal"
        result,elapsed=crash_fixture(normal)
        assert elapsed<3 and result["capture_result"]=="captured",result
        assert result["fault_address"]=="0x1" and result["signal_code"]>0
        assert result["registers"]["rip"]==result["fault_pc"] and result["registers"]["rsp"]==result["fault_sp"]
        fault=next(t for t in result["threads"] if t["fault_thread"])
        assert fault["frames"][0]["pc"]==result["fault_pc"],fault
        names=[f["function"] for f in fault["frames"]]
        assert all(any(name in f for f in names) for name in ("crash_fixture_level3","crash_fixture_level2","crash_fixture_level1")),names
        top=fault["frames"][0];module=result["modules"][top["module"]]
        assert module["name"]=="naki-crash-fixture" and module["build_id"] and top["elf_address"]
        decoded=subprocess.run(["addr2line","-f","-e",str(BIN/"naki-crash-fixture"),top["elf_address"]],
                               check=True,capture_output=True,text=True)
        assert "crash_fixture_level3" in decoded.stdout and "crash_fixture.c:" in decoded.stdout,decoded.stdout
        (RUN/"symbolized.txt").write_text(decoded.stdout)
        for mode in ("thread","locks","overflow","double"):
            result,elapsed=crash_fixture(normal,mode)
            assert elapsed<3 and result["capture_result"]=="captured",(mode,result)
            assert any(t["fault_thread"] and t["frames"] for t in result["threads"]),mode
        print("PASS real fault PC/registers, source symbolization, non-main thread, held application/stdio locks and alternate-stack overflow",flush=True)

        limited=directory/"limited"
        result,_=crash_fixture(limited,"threads",{"NAKI_CRASH_MAX_THREADS":"2","NAKI_CRASH_MAX_FRAMES":"3","NAKI_CRASH_MAX_READ_BYTES":"4096"})
        assert len(result["threads"])<=2 and result["thread_limit_hit"]
        assert all(len(t["frames"])<=3 for t in result["threads"]) and result["remote_word_read_bytes"]<=4096
        quota=directory/"quota"
        options={"NAKI_CRASH_MAX_REPORT_BYTES":"4096","NAKI_CRASH_MAX_REPORTS":"2","NAKI_CRASH_MAX_TOTAL_BYTES":"8192"}
        quota.mkdir(mode=0o700)
        sentinel=directory/"sentinel";sentinel.write_text("keep")
        (quota/"crash-1-1-1.json").symlink_to(sentinel)
        stale=quota/".crash-2-2-2.json.999999.tmp";stale.write_bytes(b"stale"*4096)
        for _ in range(5):crash_fixture(quota,"threads",extra=options)
        files=[p for p in quota.glob("crash-*.json") if not p.is_symlink()]
        assert len(files)<=2 and sum(p.stat().st_size for p in files)<=8192
        assert all(p.stat().st_size<=4096 and json.loads(p.read_text())["report_size_limit_hit"] for p in files)
        assert sentinel.read_text()=="keep" and (quota/"crash-1-1-1.json").is_symlink()
        assert not stale.exists()
        concurrent=directory/"concurrent";concurrent.mkdir(mode=0o700)
        batch=[fixture(concurrent) for _ in range(4)]
        for child,pid,collector,began in batch:
            child.wait(timeout=4);child.stdout.close()
            assert child.returncode==-signal.SIGSEGV
            wait_for(lambda:report(concurrent,pid))
        assert len(list(concurrent.glob("crash-*.json")))==4
        print("PASS thread/frame/read budgets, valid JSON size cap, directory retention and symlink exclusion",flush=True)

        # Deliberately hang the unwinder after it has attached the target; watchdog kills only capture child.
        stalled=directory/"stalled"
        result,elapsed=crash_fixture(stalled,extra={"NAKI_CRASH_TIMEOUT_MS":"600","LD_PRELOAD":str(BIN/"libnaki-unwind-stall.so")})
        assert result["capture_result"]=="capture_timeout" and elapsed<1.5,(elapsed,result)
        print("PASS external unwinder timeout detaches stopped target and produces bounded fallback report",flush=True)
        denied=directory/"denied"
        result,elapsed=crash_fixture(denied,extra={"LD_PRELOAD":str(BIN/"libnaki-unwind-stall.so"),"NAKI_TEST_DENY_PTRACE":"1"})
        assert result["capture_result"]=="captured" and not result["complete"] and result["fault_pc"]
        assert all(t["unwind_or_attach_error"]==1 and not t["frames"] for t in result["threads"])
        print("PASS ptrace denial preserves fixed context and reports incomplete stacks explicitly",flush=True)
        broken=directory/"broken"
        result,elapsed=crash_fixture(broken,extra={"LD_PRELOAD":str(BIN/"libnaki-unwind-stall.so"),"NAKI_TEST_CRASH_CAPTURE":"1"})
        assert result["capture_result"]=="capture_process_failed" and elapsed<1.5 and not result["complete"]
        print("PASS capture-child failure releases tracees and retains a fixed-context report",flush=True)

        # Collector is gone or stopped before receiving the fault: handler itself still has a deadline.
        for action in (signal.SIGKILL,signal.SIGSTOP):
            child,pid,collector,began=fixture(directory/"lost","wait",{"NAKI_CRASH_TIMEOUT_MS":"200"})
            try:
                os.kill(collector,action);time.sleep(.03)
                os.kill(pid,signal.SIGSEGV);child.wait(timeout=1)
                assert child.returncode==-signal.SIGSEGV and time.monotonic()-began<1
                if action==signal.SIGSTOP:
                    os.kill(collector,signal.SIGCONT)
                    result=wait_for(lambda:report(directory/"lost",pid))
                    assert result["capture_result"]=="target_already_exited"
            finally:
                if child.poll() is None:child.kill();child.wait()
                child.stdout.close()
        print("PASS collector death and nonresponsive collector never hold fatal process indefinitely",flush=True)

        service_reports=directory/"service"
        service=server(service_reports)
        try:
            assert http("/health")["crash_collector_pid"]>0
            job=start({"input":f"udp://127.0.0.1:{port(socket.SOCK_DGRAM)}","output":str(RUN/"worker-fault.ts")})
            worker=job["supervisor"]["worker_pid"]
            wait_for(lambda:http().get("crash_collector_pid",0)>0)
            os.kill(worker,signal.SIGSEGV);failed=finished("failed")
            assert failed["supervisor"]["exit_signal"]==signal.SIGSEGV and not failed["supervisor"]["core_dumped"]
            wr=wait_for(lambda:report(service_reports,worker))
            assert wr["role"]=="worker" and wr["job_id"]==job["job_id"] and wr["threads"]
            assert http("/health")["status"]=="ok"
            print("PASS worker SIGSEGV report and HTTP isolation without kernel core",flush=True)

            sample=RUN/"sample.mp4"
            subprocess.run(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25",
                            "-frames:v","50","-c:v","libx264","-threads","2",str(sample)],check=True,timeout=30)
            udp=port(socket.SOCK_DGRAM);output=RUN/"survive-http.ts"
            job=start({"input":f"udp://127.0.0.1:{udp}","output":str(output)})
            worker=job["supervisor"]["worker_pid"]
            sender=subprocess.Popen(["ffmpeg","-v","error","-re","-stream_loop","-1","-i",str(sample),
                                     "-c","copy","-f","mpegts",f"udp://127.0.0.1:{udp}?pkt_size=1316"],
                                    stderr=(RUN/"sender.stderr").open("w"))
            try:
                wait_for(lambda:http().get("packets_out",0)>20,timeout=12)
                before=output.stat().st_size;old=service.pid
                os.kill(old,signal.SIGSEGV);service.wait(timeout=4)
                assert service.returncode==-signal.SIGSEGV
                hr=wait_for(lambda:report(service_reports,old))
                assert hr["role"]=="http" and hr["threads"] and not dead(worker)
                time.sleep(.5);assert output.stat().st_size>before
                service=server(service_reports)
                recovered=wait_for(lambda:(s if (s:=http()).get("job_id")==job["job_id"] and s["supervisor"]["recovered"] else None))
                assert recovered["supervisor"]["worker_pid"]==worker
                http("/v1/job/stop",data={},code=202);finished("stopped")
                assert int(probe(output)["nb_read_frames"])>20
                print("PASS HTTP SIGSEGV report, uninterrupted worker output and same-PID control recovery",flush=True)
            finally:sender.terminate();sender.wait(timeout=5)
        finally:stop(service)
        # Reports contain no raw stack bytes, environment, command line or input/output URLs.
        exported=RUN/"reports";exported.mkdir()
        for report_file in directory.rglob("crash-*.json"):
            if report_file.is_symlink():continue
            data=json.loads(report_file.read_text())
            assert not any(key in data for key in ("environment","command_line","stack_memory","heap","input","output"))
            shutil.copyfile(report_file,exported/report_file.name)
        assert not list(RUN.glob("core*"))
        (RUN/"COMPLETE.json").write_text(json.dumps({"status":"passed","reports":len(list(exported.glob("*.json")))},indent=2))
        print("PASS lightweight crash collection; artifacts:",RUN,flush=True)


if __name__=="__main__":run()
