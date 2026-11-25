#!/usr/bin/env python3
"""Qualify R28 partitions, prediction, transforms and SAO against original Tencent decoder traces."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import r28_tools as tools

HERE = Path(__file__).resolve().parent
PROOF = HERE / "r28-geometry-originals.json"
TOOL_PROOF = HERE / "r28-tool-originals.json"
FILTER = HERE / "r28-geometry.awk"
CATEGORIES = ("cu_mode_geometry", "chosen_partition")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def read_counts(source):
    rows = subprocess.run(["awk", "-f", FILTER], stdin=source, stdout=subprocess.PIPE,
                          text=True, check=True).stdout
    counts = defaultdict(Counter)
    for row in rows.splitlines():
        category, key, count = row.split("\t")
        counts[category][key] = int(count)
    return counts


def archived_counts(path):
    # Piping lazily keeps decompression and reduction linear in the trace size.
    with subprocess.Popen(["gzip", "-dc", "--", path], stdout=subprocess.PIPE) as decompressor:
        try:
            result = read_counts(decompressor.stdout)
        finally:
            decompressor.stdout.close()
        if decompressor.wait():
            raise ValueError(f"invalid gzip trace {path}")
    return result


def verify_reference(directory, proof):
    combined = defaultdict(Counter)
    for source in proof["sources"]:
        path = directory / source["trace"]
        if digest(path) != source["trace_sha256"]:
            raise ValueError(f"unexpected original trace {path}")
        values = archived_counts(path)
        if values["coverage"]["pictures"] != source["frames"] or values["coverage"]["periods"] != 1:
            raise ValueError(f"incomplete original trace {path}")
        for category in CATEGORIES:
            combined[category].update(values[category])
    if dict(combined) != proof["counts"]:
        raise ValueError("measured counts differ from the committed original evidence")


def audit(stream, decoder, ffmpeg, trace, frames, output, proof, tool_proof, bench_frames, idr_frames=None):
    if frames <= 0 or output.exists():
        raise ValueError("expected a positive picture count and a new output path")
    decoder, ffmpeg, trace = map(tools.executable, (decoder, ffmpeg, trace))
    schedule = tools.idr_schedule(frames, idr_frames) if idr_frames is not None else None
    counts, pixels = tools.decode(stream, decoder, ffmpeg, trace, bench_frames, schedule)
    if counts["coverage"]["pictures"] != frames:
        raise ValueError(f"decoded {counts['coverage']['pictures']} pictures, expected {frames}")
    allowed = {**proof["counts"], **tool_proof["counts"]}
    violations = {group: {key: n for key, n in sorted(counts[group].items()) if key not in allowed[group]}
                  for group in (*CATEGORIES, *tools.CATEGORIES)}
    violations["ordered_rpl"] = {key: n for key, n in violations["ordered_rpl"].items()
                                 if not tools.reference_admitted(key, allowed["ordered_rpl"])}
    matched = pixels["vtm_sha256"] == pixels["ffmpeg_sha256"]
    passed = matched and not any(violations.values())
    result = dict(passed=passed, stream_sha256=digest(stream), frames=frames, pixels=pixels,
                  idr_periods=counts["coverage"]["periods"], violations=violations, counts=counts,
                  decoder_sha256=digest(decoder), ffmpeg_sha256=digest(ffmpeg), trace_sha256=digest(trace),
                  proof_sha256=digest(PROOF), tool_proof_sha256=digest(TOOL_PROOF),
                  filter_sha256=digest(FILTER), tool_filter_sha256=digest(HERE / "r28-tools.awk"))
    result["checker_sha256"] = digest(Path(__file__))
    result["tools_sha256"] = digest(HERE / "r28_tools.py")
    if idr_frames is not None:
        result["requested_idr_frames"] = idr_frames
    if bench_frames is not None:
        result["bench_frames_sha256"] = digest(bench_frames)
    with output.open("x") as target:
        json.dump(result, target, indent=2, sort_keys=True)
        target.write("\n")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    originals = commands.add_parser("verify-originals", help="recompute the frozen S1/S4/mini8 counts")
    originals.add_argument("directory", type=Path)
    candidate = commands.add_parser("check")
    candidate.add_argument("stream", type=Path)
    candidate.add_argument("output", type=Path)
    candidate.add_argument("--structure", required=True, type=Path)
    candidate.add_argument("--frames", required=True, type=int)
    candidate.add_argument("--ffmpeg", default="ffmpeg", help="independent VVC decoder")
    candidate.add_argument("--trace-ffmpeg", default="ffmpeg", help="FFmpeg with VVC trace_headers")
    candidate.add_argument("--bench-frames", type=Path, help="R28 benchmark frames.csv for 30 Hz CTS/DTS verification")
    candidate.add_argument("--idr-frames", help="expected comma-separated display ordinals including the startup IDR")
    args = parser.parse_args()
    tool_proof = json.loads(TOOL_PROOF.read_text())
    proof = json.loads(PROOF.read_text())
    if args.action == "verify-originals":
        verify_reference(args.directory, proof)
        if tools.original_counts(args.directory.resolve().parent, tool_proof, proof) != tool_proof["counts"]:
            raise ValueError("actual tool use differs from the committed Tencent evidence")
        print("Verified five structure/use trace pairs and three SAO header captures (248 pictures)")
        return 0
    return audit(args.stream.resolve(), args.structure, args.ffmpeg, args.trace_ffmpeg,
                 args.frames, args.output, proof, tool_proof, args.bench_frames,
                 None if args.idr_frames is None else list(map(int, args.idr_frames.split(","))))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error) if not isinstance(error, subprocess.CalledProcessError) else 2)
