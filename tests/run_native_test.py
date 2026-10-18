#!/usr/bin/env python3
"""Supply a fresh output argument to the native logger tests on every run."""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('logger', 'event-logger', 'logger-submit'))
    parser.add_argument('binary', type=pathlib.Path)
    args = parser.parse_args()
    (ROOT / 'out').mkdir(exist_ok=True)
    run = pathlib.Path(tempfile.mkdtemp(prefix=args.kind + '-', dir=ROOT / 'out'))
    argument = run / 'bounded.trace' if args.kind == 'logger' else run
    print('Native test evidence:', run, flush=True)
    return subprocess.run([str(args.binary.resolve()), str(argument)], timeout=25).returncode


if __name__ == '__main__':
    raise SystemExit(main())
