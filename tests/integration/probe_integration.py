#!/usr/bin/env python3
"""Buffered readiness: identical packet ledger/pixels, cancellation and late tracks.

Probe timings are recorded for diagnosis, never used as a shared-runner SLA.
The real RTMP/SRT, late-audio, bounded-interleave matrix lives in live_mux_integration.
"""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import hashlib
import os
import pathlib
import subprocess
import tempfile
from support.probe_budget_integration import run as budget_cases

ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE = pathlib.Path(os.environ.get("NAKI_BUILD_ROOT", pathlib.Path(os.environ.get(
    "XDG_CACHE_HOME", pathlib.Path.home() / ".cache")) / "naki-live-transcode-n9"))
BIN = pathlib.Path(os.environ.get("NAKI_SERVICE_BIN", ROOT / "out/bin"))


def run():
    result = pathlib.Path(tempfile.mkdtemp(prefix="probe-test-", dir=ROOT / "out"))
    environment = dict(os.environ, LD_LIBRARY_PATH=str(CACHE / "install/lib"))

    def command(args, capture=False):
        with (result / "commands.log").open("a") as log:
            log.write(" ".join(map(str, args)) + "\n"); log.flush()
            done = subprocess.run(list(map(str, args)), env=environment, check=True,
                                  stdout=subprocess.PIPE if capture else log, stderr=log,
                                  text=True, timeout=30)
            return json.loads(done.stdout) if capture else None

    results = []

    def all_order(ledger):
        # Timing/flags are retained for inspection too. The complete cross-stream
        # payload order is compared independently of parameter inference timing.
        return [(row[0],row[5],row[6]) for row in (line.split() for line in ledger.read_text().splitlines())]
    for codec, encoder in (("h264", "libx264"), ("hevc", "libx265")):
        for bits, rate, count in ((8, "25", 40), (10, "30000/1001", 40), (8, "25", 1)):
            name = f"{codec}-{bits}-{rate.replace('/', '_')}-{count}"
            media = result / (name + ".mp4")
            options = ["-x265-params", "pools=1:frame-threads=2:log-level=error"] if codec == "hevc" else []
            command(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=size=160x96:rate={rate}",
                     "-frames:v", count, "-c:v", encoder, "-threads", "2", "-preset", "ultrafast",
                     "-bf", "3", "-pix_fmt", "yuv420p10le" if bits == 10 else "yuv420p", *options, media])
            for fmt in ("mp4", "ts"):
                target = media
                if fmt == "ts":
                    target = result / (name + ".ts")
                    command(["ffmpeg", "-v", "error", "-i", media, "-c", "copy", "-f", "mpegts", target])
                observations = {}
                for mode in ("generic", "live"):
                    ledger = result / f"{name}-{fmt}-{mode}.ledger"
                    observations[mode] = command([BIN / "naki-probe-test", target, mode, ledger], capture=True)
                before, after = observations["generic"], observations["live"]
                for key in ("frames", "packets", "pixel_sha256", "packet_sha256"):
                    assert before[key] == after[key], (name, fmt, key, observations)
                assert after["frames"] == count, (name, fmt, after)
                assert (result / f"{name}-{fmt}-generic.ledger").read_bytes() == (
                    result / f"{name}-{fmt}-live.ledger").read_bytes(), (name, fmt, observations)
                assert all_order(result/f"{name}-{fmt}-generic.ledger.all")==all_order(
                    result/f"{name}-{fmt}-live.ledger.all"),(name,fmt,"all-stream order")
                if count > 1:
                    expected = 25 if rate == "25" else 30000 / 1001
                    assert abs(after["fps_num"] / after["fps_den"] - expected) < .001, (name, fmt, after)
                results.append({"case": name, "format": fmt, **observations})
            if codec == "h264" and count == 40 and bits == 8:
                for mode in ("invalid", "invalid-parse", "interrupt-probe"):
                    command([BIN / "naki-probe-test", media, mode, result / "unused.ledger"])
            print("PASS buffered probe packet/pixel/PTS parity:", name, flush=True)

    # Reuse existing fixtures: the initial topology is video-only; later PMT/tag
    # discovery and raw codec probing must still occur while buffered packets replay.
    from support import live_stream_fixtures
    late_ts, late_flv = live_stream_fixtures.create(result, CACHE / "install/bin/ffmpeg")
    for media in (late_ts, late_flv):
        generic=command([BIN/"naki-probe-test",media,"generic",result/(media.name+"-generic.ledger")],capture=True)
        observation = command([BIN / "naki-probe-test", media, "live", result / (media.name + ".ledger")], capture=True)
        assert observation["frames"] == 200, observation
        assert observation["final_streams"] > observation["initial_streams"], observation
        assert observation["all_packets"]==generic["all_packets"],(media.name,observation,generic)
        assert all_order(result/(media.name+"-generic.ledger.all"))==all_order(
            result/(media.name+".ledger.all")),(media.name,"audio/data payload order")
        actual_order=all_order(result/(media.name+".ledger.all"))
        assert len({row[0] for row in actual_order})>=(4 if media.suffix==".ts" else 3),media.name
        if media.suffix==".ts":
            payload=b"naki-unknown-private-data"
            assert any(size==str(len(payload)) and sha==hashlib.sha256(payload).hexdigest()
                       for _,size,sha in actual_order),"late private data payload was lost"
        results.append({"case": media.name, **observation})

    # A larger/default second video must not override the first supported video
    # that the readiness probe selected. Verify against the first video's full
    # generic ledger and independently decoded pixels.
    multiple = result / "two-videos.mp4"
    command(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=160x96:rate=25",
             "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25", "-map", "0:v", "-map", "1:v",
             "-t", "0.8", "-c:v", "libx264", "-preset", "ultrafast", "-threads", "2",
             "-disposition:v:0", "0", "-disposition:v:1", "default", multiple])
    observations = {}
    for mode in ("generic", "live"):
        observations[mode] = command([BIN / "naki-probe-test", multiple, mode,
                                      result / f"two-videos-{mode}.ledger"], capture=True)
    assert observations["live"]["frames"] == 20, observations
    assert observations["live"]["pixel_sha256"] == observations["generic"]["pixel_sha256"], observations
    assert (result / "two-videos-live.ledger").read_bytes() == (result / "two-videos-generic.ledger").read_bytes()
    results.append({"case": "deterministic-first-video", **observations})
    mp3=result/"probe-mp3.ts"
    command(["ffmpeg","-v","error","-i",result/"full.ts","-map","0:v:0","-map","0:a:0",
             "-c:v","copy","-c:a","libmp3lame","-ar","44100","-b:a","64k","-f","mpegts",mp3])
    observations={mode:command([BIN/"naki-probe-test",mp3,mode,result/f"mp3-{mode}.ledger"],capture=True)
                  for mode in ("generic","live")}
    assert observations["generic"]["frames"]==observations["live"]["frames"]==200,observations
    assert observations["generic"]["all_packets"]==observations["live"]["all_packets"],observations
    mp3_order=all_order(result/"mp3-live.ledger.all")
    assert mp3_order==all_order(result/"mp3-generic.ledger.all") and len({row[0] for row in mp3_order})==2
    results.append({"case":"mp3-all-packet-order",**observations})
    results.extend(budget_cases(result,command,BIN/"naki-probe-budget-test"))
    # Stop inside raw parsing after usable video parameters exist. A budget
    # must preserve the partial AU for subsequent reads, rather than EOF-flush it.
    raw=result/"budget-valid.h264"
    observations={mode:command([BIN/"naki-probe-test",raw,mode,result/f"resume-{mode}.ledger"],capture=True)
                  for mode in ("generic-no-clock","live-bytes")}
    assert observations["live-bytes"]["frames"]==observations["generic-no-clock"]["frames"]==200,observations
    assert observations["live-bytes"]["pixel_sha256"]==observations["generic-no-clock"]["pixel_sha256"],observations
    assert 0<observations["live-bytes"]["probe_position"]<raw.stat().st_size//2,observations
    assert (result/"resume-generic-no-clock.ledger").read_bytes()==(result/"resume-live-bytes.ledger").read_bytes()
    assert all_order(result/"resume-generic-no-clock.ledger.all")==all_order(result/"resume-live-bytes.ledger.all")
    results.append({"case":"budget-success-parser-resume",**observations})
    (result / "COMPLETE.json").write_text(json.dumps({"status": "passed", "results": results}, indent=2) + "\n")
    print("PASS buffered probe, unsupported flags, interrupt propagation and late stream discovery:", result, flush=True)


if __name__ == "__main__":
    run()
