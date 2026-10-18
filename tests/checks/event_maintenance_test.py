#!/usr/bin/env python3
"""Ownership locks, damaged tails and event-only retention are independent of PID guesses."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import fcntl
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[2]
SCRIPT=ROOT/"scripts/maintain-logs.py"


class Maintenance(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix="event-maintenance-",dir=ROOT/"out")
        self.addCleanup(self.temporary.cleanup);self.directory=pathlib.Path(self.temporary.name)
        self.instance="a"*32;self.prefix=f"worker-{self.instance}.events"

    def record(self,sequence):
        return (json.dumps(dict(schema_version=1,category="event",role="worker",instance_id=self.instance,
                                event_seq=sequence,pid=2147483647,event="fixture"))+"\n").encode()

    def invoke(self,*extra):
        result=subprocess.run([sys.executable,str(SCRIPT),"--directory",str(self.directory),"--kind","events",*extra],
                              check=True,capture_output=True,text=True,timeout=5)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def marker(self):
        path=self.directory/(self.prefix+".lock");path.write_text(self.instance+"\n");return path

    def test_active_writer_then_dead_writer_tail_recovery_without_gzip(self):
        source=self.directory/(self.prefix+".000001.jsonl.active")
        original=self.record(1)+self.record(3)+b'{"incomplete":'
        source.write_bytes(original)
        with self.marker().open("r+") as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            rows=self.invoke("--recover-orphans")
            self.assertTrue(any(row["event"]=="writer_active" for row in rows));self.assertEqual(source.read_bytes(),original)
        rows=self.invoke("--recover-orphans","--min-age-seconds","0")
        target=source.with_name(source.name[:-7])
        self.assertEqual(target.read_bytes(),self.record(1)+self.record(3));self.assertFalse(source.exists())
        quarantine=list(self.directory.glob("*.damaged-*"));self.assertEqual(len(quarantine),1)
        self.assertEqual(quarantine[0].read_bytes(),original);self.assertFalse(list(self.directory.glob("*.gz")))
        self.assertTrue(any(row["event"]=="orphan_tail_quarantined" for row in rows))

    def test_missing_or_replaced_ownership_marker_never_proves_writer_death(self):
        source=self.directory/(self.prefix+".000001.jsonl.active");source.write_bytes(self.record(1))
        self.invoke("--recover-orphans");self.assertTrue(source.exists())
        self.marker().write_text("b"*32+"\n")
        self.invoke("--recover-orphans");self.assertTrue(source.exists())
        self.marker().write_text(self.instance+"\n")
        self.invoke("--recover-orphans");self.assertFalse(source.exists())
        self.assertEqual((self.directory/(self.prefix+".000001.jsonl")).read_bytes(),self.record(1))

    def test_invalid_duplicate_and_nonfinite_records_are_quarantined(self):
        self.marker()
        for index,tail in enumerate((b'{"x":1,"x":2}\n',b'{"x":NaN}\n',b'[]\n'),1):
            source=self.directory/f"{self.prefix}.{index:06d}.jsonl.active";source.write_bytes(tail)
        self.invoke("--recover-orphans")
        self.assertEqual(len(list(self.directory.glob("*.damaged-*"))),3)
        self.assertFalse(list(self.directory.glob("*.jsonl")))

    def test_retention_does_not_touch_active_trace_unrelated_or_symlink(self):
        sealed=self.directory/(self.prefix+".000001.jsonl");sealed.write_bytes(self.record(1))
        names=(self.prefix+".000002.jsonl.active","job.trace.000001.jsonl","unrelated.jsonl")
        for name in names:(self.directory/name).write_bytes(b"preserve\n")
        link=self.directory/(self.prefix+".000003.jsonl");link.symlink_to(self.directory/"unrelated.jsonl")
        self.invoke("--max-total-bytes","1")
        self.assertFalse(sealed.exists());self.assertTrue(link.is_symlink())
        for name in names:self.assertEqual((self.directory/name).read_bytes(),b"preserve\n")

    def test_oversize_orphan_is_left_for_explicit_diagnosis(self):
        self.marker();source=self.directory/(self.prefix+".000001.jsonl.active")
        with source.open("wb") as output:output.truncate(64*1024**2+1)
        rows=self.invoke("--recover-orphans")
        self.assertTrue(source.exists());self.assertTrue(any(row["event"]=="orphan_over_limit" for row in rows))


if __name__=="__main__":unittest.main()
