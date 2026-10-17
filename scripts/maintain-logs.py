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
import uuid

SEALED = re.compile(r"^[A-Za-z0-9_-]+\.trace\.\d+\.jsonl(?:\.gz)?$")
EVENT = re.compile(r"^(?P<prefix>(?P<role>controller|worker)-(?P<instance>[0-9a-f]{32})\.events)\.\d+\.jsonl$")
EVENT_ACTIVE = re.compile(EVENT.pattern[:-1]+r"\.active$")
MAX_RECOVERY_BYTES=64*1024**2


def unique_object(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError("duplicate JSON key")
        result[key]=value
    return result


def finite_json(value):
    raise ValueError("non-finite JSON number")


def emit(event, path, **extra):
    print(json.dumps({"event": event, "path": str(path), **extra}), flush=True)


def maintained_files(directory,kind="trace"):
    pattern=EVENT if kind=="events" else SEALED
    return [p for p in directory.iterdir() if pattern.fullmatch(p.name)
            and not p.is_symlink() and p.is_file()]


def recover_events(directory):
    """A lifetime lock proves writer death; PID age/existence is never sufficient."""
    for source in sorted(directory.iterdir()):
        match=EVENT_ACTIVE.fullmatch(source.name)
        if not match or source.is_symlink():continue
        lock_path=directory/(match["prefix"]+".lock")
        try:lock_fd=os.open(lock_path,os.O_RDWR|os.O_NOFOLLOW)
        except OSError as error:
            emit("orphan_unverified",source,error=str(error));continue
        with os.fdopen(lock_fd,"r+") as ownership:
            lock_info=os.fstat(ownership.fileno())
            if not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid!=os.geteuid():
                emit("orphan_unverified",source);continue
            try:fcntl.flock(ownership,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:
                emit("writer_active",source);continue
            if ownership.read(34)!=match["instance"]+"\n":
                emit("orphan_unverified",source,reason="ownership marker mismatch");continue
            try:
                fd=os.open(source,os.O_RDONLY|os.O_NOFOLLOW)
                with os.fdopen(fd,"rb") as reader:
                    before=os.fstat(reader.fileno())
                    if not stat.S_ISREG(before.st_mode) or before.st_size>MAX_RECOVERY_BYTES:
                        emit("orphan_over_limit",source);continue
                    good=[];previous=0;damaged=False
                    while True:
                        line=reader.readline(65538)
                        if not line:break
                        try:
                            if len(line)>65537 or not line.endswith(b"\n"):raise ValueError("incomplete or overlong record")
                            row=json.loads(line,object_pairs_hook=unique_object,parse_constant=finite_json)
                            if not isinstance(row,dict) or row.get("schema_version")!=1 or row.get("category")!="event" or row.get("instance_id")!=match["instance"] or row.get("role")!=match["role"]:
                                raise ValueError("event ownership envelope mismatch")
                            sequence=row.get("event_seq")
                            if isinstance(sequence,bool) or not isinstance(sequence,int) or not previous<sequence<=2**64-1:
                                raise ValueError("invalid event sequence")
                            previous=sequence;good.append(line)
                        except (ValueError,UnicodeError,RecursionError):damaged=True;break
                current=source.stat(follow_symlinks=False)
                if (current.st_dev,current.st_ino,current.st_size,current.st_mtime_ns)!=(before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns):
                    emit("orphan_changed",source);continue
                target=source.with_name(source.name[:-len(".active")])
                if target.exists():emit("orphan_conflict",source);continue
                if not damaged:
                    os.link(source,target,follow_symlinks=False);source.unlink();emit("orphan_sealed",target,records=len(good));continue
                # Preserve the entire original outside harvesting patterns; publish only validated complete records.
                temporary=source.with_name(source.name+".recover-"+uuid.uuid4().hex)
                quarantine=source.with_name(source.name+".damaged-"+uuid.uuid4().hex)
                try:
                    if good:
                        output=os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
                        with os.fdopen(output,"wb") as writer:
                            writer.writelines(good);writer.flush();os.fsync(writer.fileno())
                        os.utime(temporary,ns=(before.st_atime_ns,before.st_mtime_ns));os.link(temporary,target)
                    os.link(source,quarantine,follow_symlinks=False);source.unlink()
                    emit("orphan_tail_quarantined",quarantine,validated_records=len(good),sealed=str(target) if good else None)
                finally:temporary.unlink(missing_ok=True)
            except OSError as error:emit("orphan_recovery_error",source,error=str(error))


def maintain(directory, min_age=60, max_age_days=7, max_bytes=2 * 1024**3,kind="trace",recover_orphans=False):
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
        if kind=="events" and recover_orphans:recover_events(directory)
        for source in maintained_files(directory,kind):
            info = source.stat(follow_symlinks=False)
            if kind=="events" or source.suffix == ".gz" or now - info.st_mtime < min_age:
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
        files = maintained_files(directory,kind)
        for path in files:
            if now - path.stat().st_mtime > max_age_days * 86400:
                path.unlink()
                emit("retention_age", path)
        files = sorted(maintained_files(directory,kind), key=lambda p: (p.stat().st_mtime, p.name))
        total = sum(p.stat().st_size for p in files)
        for path in files:
            if total <= max_bytes:
                break
            total -= path.stat().st_size
            path.unlink()
            emit("retention_size", path)
        emit("maintenance_complete", directory, sealed_bytes=total,kind=kind)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=pathlib.Path, required=True)
    parser.add_argument("--min-age-seconds", type=int, default=60)
    parser.add_argument("--max-age-days", type=int, default=7)
    parser.add_argument("--max-total-bytes", type=int, default=2 * 1024**3)
    parser.add_argument("--kind",choices=("trace","events"),default="trace")
    parser.add_argument("--recover-orphans",action="store_true",help="events only; requires an existing unlocked lifetime ownership marker")
    args = parser.parse_args()
    if args.min_age_seconds < 0 or args.max_age_days < 1 or args.max_total_bytes < 1:
        parser.error("Invalid retention limits")
    if args.recover_orphans and args.kind!="events":parser.error("Orphan recovery is only supported for process events")
    maintain(args.directory, args.min_age_seconds, args.max_age_days, args.max_total_bytes,args.kind,args.recover_orphans)
