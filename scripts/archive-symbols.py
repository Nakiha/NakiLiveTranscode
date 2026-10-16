#!/usr/bin/env python3
"""Export separate ELF debug files in GNU Build ID layout; never alter inputs."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          timeout=60, env=dict(os.environ, LC_ALL="C")).stdout


def build_id(path):
    match = re.search(r"Build ID: ([0-9a-fA-F]+)", command("readelf", "-n", str(path)))
    if not match or len(match[1]) < 8:
        raise ValueError(f"ELF has no GNU Build ID: {path}")
    return match[1].lower()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def archive(output, inputs, label, revision, dependency_key):
    output = pathlib.Path(output)
    # A fresh destination prevents a partial rebuild silently retaining old symbols.
    output.mkdir(parents=True, exist_ok=False)
    modules = []
    seen = set()
    for supplied in inputs:
        supplied = pathlib.Path(supplied)
        if not supplied.exists():
            raise ValueError(f"Missing input: {supplied}")
        paths = sorted(supplied.rglob("*")) if supplied.is_dir() else [supplied]
        for path in paths:
            if not path.is_file() or path.resolve() in seen:
                continue
            seen.add(path.resolve())
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            sections = command("readelf", "-SW", str(path))
            if ".debug_info" not in sections:
                raise ValueError(f"Input lacks DWARF debug information (already stripped?): {path}")
            identity = build_id(path)
            relative = pathlib.Path(".build-id") / identity[:2] / (identity[2:] + ".debug")
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=output) as temporary:
                debug = pathlib.Path(temporary) / "elf.debug"
                command("objcopy", "--only-keep-debug", str(path), str(debug))
                if build_id(debug) != identity:
                    raise ValueError(f"Build ID changed during extraction: {path}")
                if target.exists() and digest(target) != digest(debug):
                    raise ValueError(f"Conflicting debug data for Build ID {identity}")
                if not target.exists():
                    debug.replace(target)
            modules.append({"name": path.name, "build_id": identity, "file": relative.as_posix(),
                            "elf_sha256": digest(path), "debug_sha256": digest(target)})
    if not modules:
        raise ValueError("No ELF debug files found")
    manifest = {"schema_version": 1, "label": label, "source_revision": revision,
                "dependency_key": dependency_key, "modules": modules,
                "tools": {tool: command(tool, "--version").splitlines()[0]
                          for tool in ("readelf", "objcopy")}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Archived {len(modules)} ELF modules under {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--label", default="local")
    parser.add_argument("--source-revision", default="unknown")
    parser.add_argument("--dependency-key", default="unknown")
    parser.add_argument("inputs", nargs="+")
    args = parser.parse_args()
    try:
        archive(args.output, args.inputs, args.label, args.source_revision, args.dependency_key)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"symbol archive failed: {error}\n")
