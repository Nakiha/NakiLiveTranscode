#!/usr/bin/env python3
"""Identify local dependency sources and verify installed SDK bytes.

Content identity is deliberately independent of Git metadata: local VVenC
checkouts may be readable while their Git directory is not. Release keys use
Git separately and refuse dirty or mismatched submodules instead of guessing.
This records inputs, not a promise of byte-identical builds across toolchains.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEPENDENCIES = {
    "ffmpeg": "experiments/decoder-20261016/ffmpeg-7.0.3",
    "vvenc": "third_party/vvenc",
}
RECIPES = ("Dockerfile.ffmpeg", "scripts/build-ffmpeg.sh", "scripts/build-vvenc.sh",
           "scripts/archive-symbols.py", "scripts/sdk-identity.py")
IGNORED = {".git", ".github", ".forgejo", "doc", "docs", "tools", "tests", "test",
           "__pycache__"}
MANIFEST = pathlib.Path("share/naki/ffmpeg-build.json")
CACHE_RECEIPT = ".naki-object-keys.json"


def cache_key(build_root, prefix, component):
    """Only the atomically committed whole-SDK receipt authorizes object reuse."""
    try:
        receipt = json.loads((build_root / CACHE_RECEIPT).read_text())
        manifest = json.loads((prefix / MANIFEST).read_text())
        if receipt != {"identity": manifest["identity"], "object_keys": manifest["object_keys"]}:
            return ""
        return receipt["object_keys"][component]
    except (OSError, ValueError, KeyError, TypeError):
        return ""


def invalidate_cache(build_root, prefix):
    # Invalidate before configure/clean/compile/install, including legacy keys.
    (build_root / CACHE_RECEIPT).unlink(missing_ok=True)
    (prefix / MANIFEST).unlink(missing_ok=True)
    for name in DEPENDENCIES:
        for suffix in ("", ".pending"):
            (build_root / name / (".naki-object-key" + suffix)).unlink(missing_ok=True)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def identity(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def tree(path):
    if not path.is_dir():
        raise ValueError(f"Dependency source directory is missing: {path}")
    files = {}
    for directory, dirs, names in os.walk(path):
        dirs[:] = sorted(d for d in dirs if d not in IGNORED)
        for name in sorted(names):
            target = pathlib.Path(directory) / name
            if name == ".git" or name.endswith((".o", ".a", ".so", ".pyc")):
                continue
            if target.is_symlink():
                raise ValueError(f"Unexpected source symlink: {target}")
            files[target.relative_to(path).as_posix()] = digest(target)
    if not files:
        raise ValueError(f"Empty source tree: {path}")
    return {"sha256": identity(files), "files": len(files)}


def sources(root):
    inputs = {name: tree(root / path) for name, path in DEPENDENCIES.items()}
    recipes = {name: digest(root / name) for name in RECIPES}
    return {"schema_version": 2, "sources": inputs, "recipes": recipes,
            "identity": identity({"sources": inputs, "recipes": recipes})}


def object_key(snapshot, component):
    return identity({"source": snapshot["sources"][component], "recipes": snapshot["recipes"]})


def artifacts(prefix):
    files = {}
    for folder in ("include", "lib"):
        base = prefix / folder
        if not base.is_dir():
            raise ValueError(f"Incomplete installed SDK: {base}")
        for path in sorted(base.rglob("*")):
            if path.is_file():
                # Hash link targets too; a changed installed DSO must invalidate
                # reuse even if its versioned filename has not changed.
                files[path.relative_to(prefix).as_posix()] = digest(path)
    if not any(name.startswith("lib/libavformat.so") for name in files):
        raise ValueError("Installed SDK has no shared libavformat")
    return files


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name)
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write("\n")
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def verify(root, prefix):
    manifest = prefix / MANIFEST
    if not manifest.is_file():
        raise ValueError(f"SDK provenance is missing: {manifest}; rebuild dependencies first")
    installed = json.loads(manifest.read_text())
    if installed.get("schema_version") != 2 or installed.get("artifacts") != artifacts(prefix):
        raise ValueError("Installed SDK bytes differ from their build manifest; rebuild dependencies")
    if installed.get("object_keys") != {name: object_key(installed, name) for name in DEPENDENCIES}:
        raise ValueError("SDK has no matching content-bound object receipts; rebuild dependencies")
    available = [(root / path).is_dir() for path in DEPENDENCIES.values()]
    if all(available):
        if sources(root)["identity"] != installed["identity"]:
            raise ValueError("Dependency sources/recipe changed since SDK build; do not use NAKI_SKIP_FFMPEG=1")
    elif any(available):
        raise ValueError("Only part of the dependency sources is available; cannot verify SDK reuse")
    return installed


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise ValueError(f"Cannot verify Git source identity at {root}: {result.stderr.strip()}")
    return result.stdout.strip()


def release_key(root):
    pins = {}
    for name, relative in DEPENDENCIES.items():
        pin = git(root, "rev-parse", f"HEAD:{relative}")
        source = root / relative
        if git(source, "rev-parse", "HEAD") != pin:
            raise ValueError(f"{name} checkout does not match its parent gitlink")
        if git(source, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError(f"{name} has uncommitted sources; commit the library before making a release key")
        pins[name] = pin
    if git(root, "status", "--porcelain", "--", *RECIPES, "scripts/ffmpeg-key.sh"):
        raise ValueError("Dependency recipes are uncommitted; a release key must describe committed inputs")
    recipes = {name: digest(root / name) for name in RECIPES}
    return identity({"recipe_version": "ubuntu-24.04-linux-amd64-sdk-v3", "pins": pins, "recipes": recipes})[:24]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("snapshot", "seal", "verify", "release-key", "configure-key", "object-key", "cache-key", "invalidate-cache"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--prefix", type=pathlib.Path)
    parser.add_argument("--snapshot", type=pathlib.Path)
    parser.add_argument("--component", choices=tuple(DEPENDENCIES))
    parser.add_argument("--build-root", type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.action in ("cache-key", "invalidate-cache"):
        if not args.prefix or not args.build_root:
            parser.error("cache actions need --prefix and --build-root")
        if args.action == "cache-key":
            if not args.component:
                parser.error("cache-key needs --component")
            print(cache_key(args.build_root, args.prefix, args.component))
        else:
            invalidate_cache(args.build_root, args.prefix)
    elif args.action == "release-key":
        print(release_key(root))
    elif args.action == "object-key":
        if not args.snapshot or not args.component:
            parser.error("object-key needs --snapshot and --component")
        print(object_key(json.loads(args.snapshot.read_text()), args.component))
    elif args.action == "configure-key":
        recipe = {path: digest(root / path) for path in RECIPES}
        recipe["configure"] = digest(root / DEPENDENCIES["ffmpeg"] / "configure")
        recipe["source_directory"] = str(root)
        recipe["environment"] = {name: os.environ.get(name, "") for name in
                                 ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS")}
        print(identity(recipe))
    elif args.action == "snapshot":
        if not args.snapshot:
            parser.error("snapshot needs --snapshot")
        atomic_json(args.snapshot, sources(root))
    elif args.action == "verify":
        if not args.prefix:
            parser.error("verify needs --prefix")
        value = verify(root, args.prefix)
        print(f"Verified SDK {value['identity']}")
    else:
        if not args.prefix or not args.snapshot or not args.build_root:
            parser.error("seal needs --prefix, --snapshot and --build-root")
        # Direct seal callers must also retire the previous commit before any
        # validation can fail. Pending component results are not reusable keys.
        (args.build_root / CACHE_RECEIPT).unlink(missing_ok=True)
        (args.prefix / MANIFEST).unlink(missing_ok=True)
        value = json.loads(args.snapshot.read_text())
        if value != sources(root):
            raise ValueError("Dependency inputs changed during compilation; SDK has not been sealed")
        value["object_keys"] = {name: object_key(value, name) for name in DEPENDENCIES}
        for name, key in value["object_keys"].items():
            receipt = args.build_root / name / ".naki-object-key.pending"
            if not receipt.is_file() or receipt.read_text().strip() != key:
                raise ValueError(f"No successful content-bound {name} build receipt; refusing to seal")
        value["artifacts"] = artifacts(args.prefix)
        value["toolchain"] = {
            name: subprocess.check_output([name, "--version"], text=True, timeout=10).splitlines()[0]
            for name in ("cc", "c++", "cmake", "make")}
        atomic_json(args.prefix / MANIFEST, value)
        # Last atomic operation commits both components together. A failure or
        # uncatchable interruption before this leaves no reusable object key.
        atomic_json(args.build_root / CACHE_RECEIPT,
                    {"identity": value["identity"], "object_keys": value["object_keys"]})
        print(f"Sealed SDK {value['identity']}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"SDK identity check failed: {error}")
