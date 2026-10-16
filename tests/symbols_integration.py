#!/usr/bin/env python3
"""Real PIE crash -> separate symbols -> source lines, with mismatch/failure checks."""
import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("NAKI_SERVICE_BIN", ROOT / "out/bin"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(ffmpeg_symbols):
    archiver = load("archive-symbols")
    resolver = load("symbolize-crash")
    (ROOT / "out").mkdir(exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix="symbols-test-", dir=ROOT / "out"))
    archive = evidence / "symbols"
    archiver.archive(archive, [BIN / "naki-crash-fixture", BIN / "naki-live-worker"], "test", "test", "test")
    with tempfile.TemporaryDirectory(prefix="naki-symbols-") as private:
        process = subprocess.run([str(BIN / "naki-crash-fixture"), "segv"],
                                 env=dict(os.environ, NAKI_CRASH_DIR=private),
                                 capture_output=True, timeout=6)
        assert process.returncode == -signal.SIGSEGV, process
        reports = list(pathlib.Path(private).glob("crash-*.json"))
        assert len(reports) == 1
        report_path = evidence / "crash.json"
        shutil.copyfile(reports[0], report_path)
    original = json.loads(report_path.read_text())
    decoded = resolver.symbolize(original, [archive])
    fault = next(thread for thread in decoded["threads"] if thread["fault_thread"])
    top = fault["frames"][0]["symbolization"]
    assert top["status"] == "resolved", top
    assert any(item["function"] == "crash_fixture_level3" and "crash_fixture.c:" in item["location"]
               for item in top["inline_chain"]), top
    (evidence / "symbolized.txt").write_text(resolver.format_text(decoded))
    subprocess.run(["python3", str(ROOT / "scripts/symbolize-crash.py"), str(report_path),
                    "--symbols", str(archive), "--require-module", "naki-crash-fixture",
                    "--format", "json", "--output", str(evidence / "symbolized.json")], check=True)
    identity = original["modules"][fault["frames"][0]["module"]]["build_id"]
    relative = pathlib.Path(".build-id") / identity[:2] / (identity[2:] + ".debug")
    stripped = evidence / "runtime-fixture"
    shutil.copyfile(BIN / "naki-crash-fixture", stripped)
    subprocess.run(["strip", "--strip-debug", str(stripped)], check=True)
    assert archiver.build_id(stripped) == identity
    assert ".debug_info" not in archiver.command("readelf", "-SW", str(stripped))
    try:
        archiver.archive(evidence / "already-stripped", [stripped], "test", "test", "test")
        raise AssertionError("stripped input accepted")
    except ValueError as error:
        assert "lacks DWARF" in str(error)
    empty = evidence / "empty"; empty.mkdir()
    missing = resolver.symbolize(original, [empty])
    assert missing["offline_symbolization"]["resolved_frames"] == 0
    assert any(frame["symbolization"]["status"] == "missing_symbols"
               for thread in missing["threads"] for frame in thread["frames"])
    bad = evidence / "wrong-symbols" / relative; bad.parent.mkdir(parents=True)
    shutil.copyfile(BIN / "naki-live-worker", bad)
    try:
        resolver.symbolize(original, [evidence / "wrong-symbols"])
        raise AssertionError("mismatched Build ID accepted")
    except ValueError as error:
        assert "Build ID mismatch" in str(error)
    malicious = json.loads(report_path.read_text())
    malicious["modules"][0]["build_id"] = "../../bad"
    assert resolver.symbolize(malicious, [archive])
    malicious["threads"][0]["frames"][0]["elf_address"] = "0x1;echo unwanted"
    try:
        resolver.symbolize(malicious, [archive])
        raise AssertionError("invalid address accepted")
    except ValueError:
        pass
    if ffmpeg_symbols:
        manifest = json.loads((ffmpeg_symbols / "manifest.json").read_text())
        module = next(module for module in manifest["modules"] if module["name"].startswith("libavcodec.so"))
        # Known exported function's ELF address tests library DWARF independent of ASLR.
        prefix = pathlib.Path(os.environ.get("NAKI_BUILD_ROOT", "/opt/naki-build")) / "install/lib/libavcodec.so"
        symbols = archiver.command("nm", "-D", "--defined-only", str(prefix))
        location = next(line.split()[0] for line in symbols.splitlines() if " avcodec_version@@" in line)
        sample = dict(original, modules=[dict(module, id=0)], threads=[{
            "tid": 1, "frames": [{"module": 0, "elf_address": "0x" + location}]}])
        resolved = resolver.symbolize(sample, [ffmpeg_symbols])
        frame = resolved["threads"][0]["frames"][0]["symbolization"]
        assert frame["status"] == "resolved" and any("version.c:" in item["location"] for item in frame["inline_chain"]), frame
    print("PASS exact Build ID, stripped runtime, real PIE fault/source line, missing/mismatched symbols and invalid input; FFmpeg DWARF checked:", bool(ffmpeg_symbols), "; artifacts:", evidence)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffmpeg-symbols", type=pathlib.Path)
    run(parser.parse_args().ffmpeg_symbols)
