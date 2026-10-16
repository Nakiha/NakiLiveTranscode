#!/usr/bin/env python3
"""Resolve bounded crash JSON with exact Build ID symbols, entirely offline."""
import argparse
import copy
import json
import os
import pathlib
import re
import subprocess
import sys


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=15,
                          env=dict(os.environ, LC_ALL="C", DEBUGINFOD_URLS="")).stdout


def address(value):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        raise ValueError(f"Invalid ELF address: {value!r}")
    return int(value, 16)


def symbolize(report, roots):
    result = copy.deepcopy(report)
    if report.get("schema_version") != 1 or report.get("event") != "crash_report":
        raise ValueError("Expected schema_version=1 crash_report")
    modules = result.get("modules", [])
    threads = result.get("threads", [])
    if not isinstance(modules, list) or len(modules) > 256 or not isinstance(threads, list) or len(threads) > 256:
        raise ValueError("Invalid module/thread limits")
    resolved = {}
    for index, module in enumerate(modules):
        identity = module.get("build_id", "")
        if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-fA-F]{8,128}", identity):
            resolved[index] = (None, "missing_build_id")
            continue
        identity = identity.lower()
        relative = pathlib.Path(".build-id") / identity[:2] / (identity[2:] + ".debug")
        candidates = [root / relative for root in roots if (root / relative).is_file()]
        if not candidates:
            resolved[index] = (None, "missing_symbols")
            continue
        for candidate in candidates:
            match = re.search(r"Build ID: ([0-9a-fA-F]+)", command("readelf", "-n", str(candidate)))
            if not match or match[1].lower() != identity:
                raise ValueError(f"Build ID mismatch for {identity}: {candidate}")
        resolved[index] = (candidates[0], "matched")
    count = 0
    cache = {}
    for thread in threads:
        frames = thread.get("frames", [])
        if not isinstance(frames, list) or len(frames) > 512:
            raise ValueError("Invalid frame limit")
        for frame in frames:
            module_index = frame.get("module", -1)
            if not isinstance(module_index, int):
                raise ValueError("Invalid module index")
            debug, status = resolved.get(module_index, (None, "unknown_module"))
            frame["symbolization"] = {"status": status}
            if frame.get("elf_address") is None:
                frame["symbolization"]["status"] = "missing_elf_address"
                continue
            elf_address = address(frame["elf_address"])
            if debug is None:
                continue
            # Collector already subtracts ELF load bias. Keep PCs exact: libunwind
            # may mix return PCs and signal frames; never guess a PC-1 adjustment.
            key = (str(debug), elf_address)
            if key not in cache:
                lines = command("addr2line", "-f", "-C", "-i", "-e", str(debug), hex(elf_address)).splitlines()
                cache[key] = [{"function": lines[i], "location": lines[i + 1]}
                              for i in range(0, len(lines) - 1, 2)]
            locations = cache[key]
            has_source = any(re.search(r":\d+(?:\s|$)", item["location"]) and not item["location"].startswith("??")
                             and not re.search(r":0(?:\s|$)", item["location"]) for item in locations)
            frame["symbolization"] = {"status": "resolved" if has_source else "no_source_line",
                                      "build_id": modules[module_index]["build_id"],
                                      "lookup_address": hex(elf_address), "inline_chain": locations}
            count += int(has_source)
    result["offline_symbolization"] = {"schema_version": 1, "resolved_frames": count,
                                        "pc_policy": "exact_elf_pc", "network_used": False}
    return result


def format_text(report):
    lines = [f"{report['role']} pid={report['pid']} signal={report['signal']} capture={report['capture_result']}",
             f"complete={report.get('complete')} truncated={report.get('truncated')} full_core={report.get('full_core')}"]
    for thread in report.get("threads", []):
        lines.append(f"Thread {thread['tid']} fault={thread.get('fault_thread')} unwind_error={thread.get('unwind_or_attach_error')}")
        for index, frame in enumerate(thread.get("frames", [])):
            symbol = frame["symbolization"]
            lines.append(f"  #{index} {frame.get('pc')} ELF={frame.get('elf_address')} [{symbol['status']}]")
            for item in symbol.get("inline_chain", []):
                lines.append(f"      {item['function']} at {item['location']}")
    lines.append(f"Resolved source frames: {report['offline_symbolization']['resolved_frames']}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("--symbols", action="append", type=pathlib.Path, required=True,
                        help="Archive root containing .build-id; repeat for application and FFmpeg")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--require-module", action="append", default=[],
                        help="Fail unless this module has a frame resolved to a source line")
    args = parser.parse_args()
    try:
        with args.report.open("rb") as stream:
            data = stream.read(2 * 1024 * 1024 + 1)
        if len(data) > 2 * 1024 * 1024:
            raise ValueError("Report exceeds 2 MiB")
        result = symbolize(json.loads(data), args.symbols)
        for required in args.require_module:
            indices = {i for i, module in enumerate(result["modules"]) if module.get("name") == required}
            if not any(frame.get("module") in indices and frame["symbolization"]["status"] == "resolved"
                       for thread in result["threads"] for frame in thread.get("frames", [])):
                raise ValueError(f"No source line resolved for required module: {required}")
        rendered = json.dumps(result, indent=2) + "\n" if args.format == "json" else format_text(result)
        if args.output:
            args.output.write_text(rendered)
        else:
            sys.stdout.write(rendered)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"symbolization failed: {error}\n")


if __name__ == "__main__":
    main()
