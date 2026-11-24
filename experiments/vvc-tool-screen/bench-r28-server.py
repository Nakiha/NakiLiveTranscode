#!/usr/bin/env python3
"""Compare R28 closed-loop encoding and R26 at the resulting per-picture QPs."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run_case(directory, binary, library, command, cpus):
    directory.mkdir()
    log = directory / "bench.log"
    with log.open("w") as output:
        subprocess.run(["taskset", "-c", cpus, str(binary), *map(str, command)],
                       cwd=directory, env=dict(os.environ, LD_LIBRARY_PATH=str(library)),
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    with (directory / "frames.csv").open() as trace:
        rows = list(csv.DictReader(trace))
    assert sorted(int(x["frame"]) for x in rows) == list(range(len(rows)))
    assert all(int(b["dts_tick"])-int(a["dts_tick"]) == 3000 for a, b in zip(rows, rows[1:]))
    write = [int(x["write_us"]) for x in rows]
    prefix = [0]
    for row in rows:
        prefix.append(prefix[-1] + 8 * int(row["bytes"]))
    start = 0
    wall_peak = 0
    for end, timestamp in enumerate(write):
        while write[start] <= timestamp - 5000000:
            start += 1
        wall_peak = max(wall_peak, (prefix[end+1]-prefix[start])/5)
    count = len(rows)
    latency = sorted(int(x["residence_us"]) for x in rows)
    summary = dict(frames=count, fps=count*1e6/write[-1],
                   slowest_150_fps=min(150e6/(write[i+150]-write[i]) for i in range(count-150)),
                   video_bps=prefix[-1]*30/count,
                   peak_video_150_bps=max((prefix[i+150]-prefix[i])/5 for i in range(count-149)),
                   peak_written_5s_bps=wall_peak, submit_to_au_p95_ms=latency[math.ceil(count*.95)-1]/1000,
                   peak_future_frames=max(int(x["future_frames"]) for x in rows),
                   cpu_ms=float(re.search(r"cpu_ms=([\d.]+)", log.read_text())[1]),
                   stream_sha256=digest(directory / "video.266"))
    return rows, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="540x960, 8-bit planar I420 at 30 fps")
    parser.add_argument("--kit", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--tier", choices=("live", "quality"), default="live")
    parser.add_argument("--threads", nargs="+", type=int, default=[8])
    parser.add_argument("--pictures", nargs="+", type=int, choices=(4, 8), default=[4, 8])
    parser.add_argument("--cpus", required=True, help="Linux taskset CPU list for this single stream")
    parser.add_argument("--crf", type=float, default=29)
    parser.add_argument("--maxrate", type=int, default=2100000)
    parser.add_argument("--burst-ms", type=int, default=300)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--paced", action="store_true", help="30 fps wall-clock input and write feedback")
    parser.add_argument("--with-r26", action="store_true", help="also replay actual R28 QPs at full speed on R26")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    src = args.input.resolve()
    if not 151 <= args.frames <= 1800 or src.stat().st_size < args.frames*540*960*3//2:
        parser.error("supply 151 to 1800 complete 540x960 I420 frames")
    if not all(2 <= n <= 64 for n in args.threads):
        parser.error("--threads accepts 2 through 64")
    kit = args.kit.resolve()
    directory = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="r28-", dir=".")).resolve()
    if args.out:
        directory.mkdir(parents=True, exist_ok=False)
    report = dict(host=platform.node(), kernel=platform.release(), cpus=args.cpus,
                  cpu_model=next((x.split(":",1)[1].strip() for x in Path("/proc/cpuinfo").read_text().splitlines()
                                  if x.startswith("model name")), None),
                  source_sha256=digest(src), tier=args.tier, crf=args.crf, maxrate=args.maxrate,
                  burst_ms=args.burst_ms, clock="paced_wall" if args.paced else "unpaced_program_clock",
                  results=[])
    base = (kit / "profiles" / (args.tier+".cfg")).read_text()
    for threads in args.threads:
        for pictures in args.pictures:
            name = f"{threads}t-{pictures}p"
            profile = directory / (name+".cfg")
            profile.write_text(base.replace("Threads=8", f"Threads={threads}")
                               .replace("MaxParallelFrames=4", f"MaxParallelFrames={pictures}").rstrip()
                               + "\nLiveGopPipeline=3\nLiveNative8Rows=1\n")
            cmd = [src, profile, args.crf, args.frames, "1" if args.paced else "feedback-fps",
                   args.maxrate, "measure", "-", "5", "anchor-feedback", "0.4", "--ccrf=balanced",
                   f"--burst-ms={args.burst_ms}", "--history-ms=0", "--reserve-groups=0", "--anchor-margin-pct=0"]
            rows, result = run_case(directory/name, kit/"native/bin/vvenc-live-bench",
                                    kit/"native/lib", cmd, args.cpus)
            result.update(threads=threads, pictures=pictures, arm="r28")
            report["results"].append(result)
            print(name, "R28", round(result["fps"], 2), "fps", flush=True)
            if args.with_r26:
                trace = directory / (name+"-qps.txt")
                trace.write_text("".join(x["requested_qp"]+"\n" for x in sorted(rows,key=lambda x:int(x["frame"]))))
                previous = directory / (name+"-r26.cfg")
                previous.write_text(base.replace("Threads=8", f"Threads={threads}"))
                replay = [src, previous, int(args.crf), args.frames, 0, 0, "--qp-replay="+str(trace)]
                _, baseline = run_case(directory/(name+"-r26"), kit/"r26/bin/vvenc-live-bench",
                                       kit/"r26/lib", replay, args.cpus)
                baseline.update(threads=threads, pictures=4, arm="r26_qp_replay",
                                paired_r28=name, identical_stream=baseline["stream_sha256"]==result["stream_sha256"])
                report["results"].append(baseline)
                print(name, "R26 replay", round(baseline["fps"],2), "fps, identical stream", baseline["identical_stream"], flush=True)
            (directory/"summary.json").write_text(json.dumps(report, indent=2)+"\n")
    print(directory/"summary.json")


if __name__ == "__main__":
    main()
