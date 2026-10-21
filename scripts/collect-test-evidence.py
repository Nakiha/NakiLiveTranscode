#!/usr/bin/env python3
"""Run a CI check and preserve its actual output inventory, including failures."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile


def collect(root, entries, destination):
    root = root.resolve()
    pending = list(entries)
    selected = set()
    errors = []
    # Summary references are authoritative even when the suite added no new path.
    for entry in list(entries):
        summary = entry / 'summary.json'
        if summary.is_file() and not summary.is_symlink():
            try:
                for step in json.loads(summary.read_text()).get('steps', []):
                    pending.extend(pathlib.Path(p) for p in step.get('artifacts', []))
            except (OSError, ValueError, TypeError) as error:
                errors.append(f'{summary}: {error}')
    for entry in pending:
        if not entry.is_absolute():
            entry = root.parent / entry
        if entry.is_symlink() or not entry.resolve().is_relative_to(root) or entry.resolve() == root:
            errors.append(f'Unsafe evidence path: {entry}')
        elif not entry.exists():
            errors.append(f'Missing referenced evidence: {entry}')
        else:
            selected.add(entry)
    inventory = []
    metadata = []
    total = 0
    for entry in sorted(selected):
        paths = [entry] if entry.is_file() else sorted(entry.rglob('*'))
        for path in paths:
            if path.is_symlink():
                # Deliberate symlinks are part of logger/crash safety fixtures.
                # Preserve their identity as data; never copy or follow them.
                metadata.append(dict(path=str(path.relative_to(root)), type='symlink', target=os.readlink(path)))
                continue
            if not path.resolve().is_relative_to(root):
                errors.append(f'Unsafe evidence member: {path}')
                continue
            if path.is_dir():
                continue
            if path.name.endswith('.jsonl.active'):
                # The logger has not sealed this segment. Preserve the fact
                # that it existed, never externally retain/compress its bytes.
                metadata.append(dict(path=str(path.relative_to(root)), type='unsealed_trace', bytes=path.stat().st_size))
                continue
            if not path.is_file():
                metadata.append(dict(path=str(path.relative_to(root)), type='special', mode=stat.S_IFMT(path.stat().st_mode)))
                continue
            relative = path.relative_to(root)
            target = destination / 'out' / relative
            if target.exists():
                continue
            size = path.stat().st_size
            total += size
            if total > 16 * 1024**3 or len(inventory) >= 100000:
                raise ValueError('Evidence exceeds file/byte collection bounds')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            digest = hashlib.sha256()
            with target.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            inventory.append(dict(source=str(path), path=str(target.relative_to(destination)),
                                  bytes=size, sha256=digest.hexdigest()))
    return dict(files=inventory, metadata=metadata, errors=errors, status='failed' if errors else 'complete')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / 'out')
    parser.add_argument('--destination', type=pathlib.Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('A test command is required')
    args.root.mkdir(parents=True, exist_ok=True)
    args.destination.mkdir(parents=True, exist_ok=True)
    batch = pathlib.Path(tempfile.mkdtemp(prefix='evidence-', dir=args.destination))
    before = set(args.root.iterdir())
    report = dict(command=command, test_exit_code=127, status='failed', files=[], errors=[])
    try:
        with (batch / 'command.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for line in process.stdout:
                log.write(line)
                log.flush()
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
            report['test_exit_code'] = process.wait()
    except OSError as error:
        report['errors'].append(str(error))
    try:
        entries = set(args.root.iterdir()) - before
        entries = {p for p in entries if p.resolve() != args.destination.resolve() and not batch.is_relative_to(p)}
        collected = collect(args.root, entries, batch)
        report['files'] = collected['files']
        report['metadata'] = collected['metadata']
        report['errors'].extend(collected['errors'])
        report['status'] = 'failed' if report['errors'] else 'complete'
    except (OSError, ValueError) as error:
        report['errors'].append(str(error))
    finally:
        (batch / 'collection.json').write_text(json.dumps(report, indent=2) + '\n')
        for path in [batch, *batch.rglob('*')]:
            os.chmod(path, 0o755 if path.is_dir() else 0o644)
    return report['test_exit_code'] if report['test_exit_code'] else int(report['status'] != 'complete')


if __name__ == '__main__':
    raise SystemExit(main())
