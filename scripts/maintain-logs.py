#!/usr/bin/env python3
"""Compress immutable trace segments and enforce bounded retention, outside C."""
import argparse
import fcntl
import gzip
import json
import os
import pathlib
import re
import shutil
import stat
import time

SEALED = re.compile(r"^[A-Za-z0-9_-]+\.trace\.\d+\.jsonl(?:\.gz)?$")


def emit(event, path, **extra):
    print(json.dumps({"event": event, "path": str(path), **extra}), flush=True)


def maintained_files(directory):
    return [p for p in directory.iterdir() if SEALED.fullmatch(p.name)
            and not p.is_symlink() and p.is_file()]


def maintain(directory, min_age=60, max_age_days=7, max_bytes=2 * 1024**3):
    directory = directory.resolve(strict=True)
    if not directory.is_dir():
        raise ValueError("Log destination must be a directory")
    # The same persistent inode serializes concurrent host/container invocations.
    lock_fd = os.open(directory / ".maintenance.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, "a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            emit("maintenance_busy", directory)
            return
        now = time.time()
        for source in maintained_files(directory):
            info = source.stat(follow_symlinks=False)
            if source.suffix == ".gz" or now - info.st_mtime < min_age:
                continue
            target = source.with_name(source.name + ".gz")
            temporary = source.with_name(source.name + f".gz.tmp-{os.getpid()}")
            if target.exists():
                emit("compression_conflict", source)
                continue
            try:
                input_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
                with os.fdopen(input_fd, "rb") as reader:
                    original = os.fstat(reader.fileno())
                    if not stat.S_ISREG(original.st_mode):
                        continue
                    output_fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                    with os.fdopen(output_fd, "wb") as writer:
                        with gzip.GzipFile(fileobj=writer, mode="wb", filename="", compresslevel=1,
                                           mtime=int(original.st_mtime)) as compressor:
                            shutil.copyfileobj(reader, compressor, length=1024 * 1024)
                        writer.flush()
                        os.fsync(writer.fileno())
                os.utime(temporary, (original.st_atime, original.st_mtime))
                # Atomic publish without overwriting a concurrently created target.
                os.link(temporary, target)
                current = source.stat(follow_symlinks=False)
                if (current.st_ino, current.st_size, current.st_mtime_ns) == (original.st_ino, original.st_size, original.st_mtime_ns):
                    source.unlink()
                emit("compressed", target, original_bytes=original.st_size)
            except OSError as error:
                emit("compression_error", source, error=str(error))
            finally:
                temporary.unlink(missing_ok=True)
        files = maintained_files(directory)
        for path in files:
            if now - path.stat().st_mtime > max_age_days * 86400:
                path.unlink()
                emit("retention_age", path)
        files = sorted(maintained_files(directory), key=lambda p: (p.stat().st_mtime, p.name))
        total = sum(p.stat().st_size for p in files)
        for path in files:
            if total <= max_bytes:
                break
            total -= path.stat().st_size
            path.unlink()
            emit("retention_size", path)
        emit("maintenance_complete", directory, sealed_bytes=total)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=pathlib.Path, required=True)
    parser.add_argument("--min-age-seconds", type=int, default=60)
    parser.add_argument("--max-age-days", type=int, default=7)
    parser.add_argument("--max-total-bytes", type=int, default=2 * 1024**3)
    args = parser.parse_args()
    if args.min_age_seconds < 0 or args.max_age_days < 1 or args.max_total_bytes < 1:
        parser.error("Invalid retention limits")
    maintain(args.directory, args.min_age_seconds, args.max_age_days, args.max_total_bytes)
