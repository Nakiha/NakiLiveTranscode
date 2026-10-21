#!/usr/bin/env python3
"""Generated-media regression for the fork's async API, independent of frozen fixtures."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import csv
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = pathlib.Path(os.environ.get("NAKI_DECODER_PROBE_SOURCE",
    ROOT / "experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async"))
CACHE = pathlib.Path(os.environ.get("NAKI_BUILD_ROOT", pathlib.Path(os.environ.get(
    "XDG_CACHE_HOME", pathlib.Path.home() / ".cache")) / "naki-live-transcode-n9"))
PREFIX = CACHE / "install"


def run():
    (ROOT / "out").mkdir(exist_ok=True)
    result = pathlib.Path(tempfile.mkdtemp(prefix="decoder-test-", dir=ROOT / "out"))
    environment = dict(os.environ, PKG_CONFIG_PATH=str(PREFIX / "lib/pkgconfig"),
                       LD_LIBRARY_PATH=str(PREFIX / "lib"))

    def command(args, extra=None, timeout=45):
        with (result / "commands.log").open("a") as log:
            log.write(" ".join(map(str, args)) + "\n"); log.flush()
            subprocess.run(list(map(str, args)), env=dict(environment, **(extra or {})),
                           stdout=log, stderr=log, check=True, timeout=timeout)

    libraries = subprocess.check_output(["pkg-config", "--libs", "libavformat", "libavcodec", "libavutil"],
                                        env=environment, text=True).split()
    for name in ("async_probe", "auto_probe", "auto_lifetime_test"):
        command(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(PREFIX / "include"),
                 SOURCE / (name + ".c"), "-o", result / name, *libraries])
    for codec, encoder in (("h264", "libx264"), ("hevc", "libx265")):
        for reorder in (0, 3):
            media = result / f"{codec}-b{reorder}.mp4"
            command(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
                     "-frames:v", "50", "-c:v", encoder, "-threads", "2", "-preset", "ultrafast", "-bf", str(reorder),
                     *(["-x265-params", "pools=2:frame-threads=2:log-level=error"] if codec == "hevc" else []), media])
            for threads in (2, 12):
                for mode in ("legacy", "async"):
                    command([result / "async_probe", media, threads, 0, mode,
                             result / f"{codec}-b{reorder}-t{threads}-{mode}"], {"FFMPEG_LIVE_AUTO_MIN": "2"})
                expected = (result / f"{codec}-b{reorder}-t{threads}-legacy.sha256").read_bytes()
                assert len(expected.splitlines()) == 50
                assert expected == (result / f"{codec}-b{reorder}-t{threads}-async.sha256").read_bytes()
        media = result / f"{codec}-b0.mp4"
        expected = (result / f"{codec}-b0-t12-legacy.sha256").read_bytes()
        for mode in ("gate", "pause", "error", "flush", "api", "close"):
            target = result / f"{codec}-{mode}"
            command([result / "async_probe", media, 12, 0, mode, target, 1, 20000 if mode in ("flush", "close") else 0, 1])
            if mode != "close":
                assert target.with_suffix(".sha256").read_bytes() == expected, (codec, mode)
        target = result / f"{codec}-replay"
        command([result / "async_probe", media, 12, 0, "async", target, 3, 0, 1], {"FFMPEG_LIVE_AUTO_MIN": "2"})
        assert target.with_suffix(".sha256").read_bytes() == expected * 3
        for mode, slots in (("fixed", 12), ("auto", 1), ("auto", 2), ("auto", 4)):
            target = result / f"{codec}-{mode}-{slots}"
            command([result / "auto_probe", media, 12, 1, slots, "-", mode, 0, 3, target, 1, 0])
            metadata = json.loads(target.with_suffix(".json").read_text())
            assert metadata["frames"] == 150
            for stream in range(3):
                assert pathlib.Path(str(target) + f"-s{stream}.sha256").read_bytes() == (
                    result / f"{codec}-fixed-12-s0.sha256").read_bytes()
            if mode == "auto":
                with pathlib.Path(str(target) + ".control.csv").open() as file:
                    for row in csv.DictReader(file):
                        assert 0 <= int(row["busy"]) <= int(row["leased"]) <= slots, row
        print(f"PASS {codec}: generated B=0/3 pixel/metadata parity, lifecycle, capacity-one replay and three-owner budgets", flush=True)
    for codec in ("h264", "hevc"):
        command([result / "auto_lifetime_test", result / f"{codec}-b0.mp4"], timeout=90)
    from support.input_reorder_matrix import run_matrix
    run_matrix(result / "async_probe", result / "input-reorder", environment)
    (result / "COMPLETE.json").write_text(json.dumps({"status": "passed", "ffmpeg_prefix": str(PREFIX)}) + "\n")
    print("PASS decoder regression; artifacts:", result, flush=True)


if __name__ == "__main__":
    run()
