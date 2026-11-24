#!/usr/bin/env python3
"""Small trace regressions for the role, leaf mode and completeness checks."""
import json
from pathlib import Path
import subprocess
import unittest

HERE = Path(__file__).resolve().parent
PROOF = json.loads((HERE / "r28-geometry-originals.json").read_text())["counts"]


def picture(order, poc, intra, nal, leaves):
    header = f"V H {order} {poc} 0 {2 if intra else 0} {nal} 0 8 8 0 0"
    node = f"V N {order} 0 0 0 8 8 0 0 1"
    return "\n".join([header, node, *[f"V C {order} 0 {leaf}" for leaf in leaves]]) + "\n"


def reduce(trace):
    return subprocess.run(["awk", "-f", HERE / "r28-geometry.awk"], input=trace, text=True,
                          capture_output=True)


class GeometryTests(unittest.TestCase):
    idr = picture(0, 93, True, 7, ["0 0 8 8 1"])

    def test_nonzero_idr_and_leaf_mode(self):
        for intra in (0, 1):
            start = picture(1, 94, False, 0, [f"0 0 4 8 {intra}", f"4 0 4 8 {intra}"])
            result = reduce(self.idr + start)
            self.assertEqual(result.returncode, 0, result.stderr)
            key = f"start+1/{'intra' if intra else 'inter'}/4x8"
            self.assertIn("cu_mode_geometry\t" + key + "\t2", result.stdout)
            self.assertEqual(key in PROOF["cu_mode_geometry"], not intra)

    def test_missing_or_incomplete_trace(self):
        for trace in ("", self.idr + picture(1, 94, False, 0, ["0 0 4 8 0"]),
                      self.idr + picture(1, 93, False, 0, ["0 0 8 8 0"])):
            self.assertNotEqual(reduce(trace).returncode, 0)


if __name__ == "__main__":
    unittest.main()
