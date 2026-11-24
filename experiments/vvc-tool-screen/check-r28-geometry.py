#!/usr/bin/env python3
"""Check actual Tencent same-role luma partitions with the instrumented VTM decoder."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROOF = HERE / "r28-geometry-originals.json"
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


def audit(stream, decoder, frames, output, proof):
    if frames <= 0 or output.exists():
        raise ValueError("expected a positive picture count and a new output path")
    with subprocess.Popen([decoder, "-b", stream], stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE) as process:
        try:
            counts = read_counts(process.stderr)
        finally:
            process.stderr.close()
        if process.wait():
            raise ValueError("VTM decoding failed")
    if counts["coverage"]["pictures"] != frames:
        raise ValueError(f"decoded {counts['coverage']['pictures']} pictures, expected {frames}")
    violations = {category: {k: n for k, n in sorted(counts[category].items())
                             if k not in proof["counts"][category]} for category in CATEGORIES}
    passed = not any(violations.values())
    result = dict(passed=passed, stream_sha256=digest(stream), decoder_sha256=digest(decoder),
                  proof_sha256=digest(PROOF), filter_sha256=digest(FILTER), frames=frames,
                  idr_periods=counts["coverage"]["periods"], violations=violations, counts=counts)
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
    args = parser.parse_args()
    proof = json.loads(PROOF.read_text())
    if args.action == "verify-originals":
        verify_reference(args.directory, proof)
        print("Verified all five original structure traces")
        return 0
    return audit(args.stream.resolve(), args.structure.resolve(), args.frames, args.output, proof)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error) if not isinstance(error, subprocess.CalledProcessError) else 2)
