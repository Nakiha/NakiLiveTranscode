#!/usr/bin/env python3
"""Small trace regressions for the role, leaf mode and completeness checks."""
import json
from pathlib import Path
import subprocess
import unittest

import r28_tools as tools

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


class ToolGateTests(unittest.TestCase):
    proof = json.loads((HERE / "r28-tool-originals.json").read_text())["counts"]

    def test_observed_indices_precisions_and_transform_planes(self):
        cases = (
            ("parameter_by_shape", "merge_index/start+2/8x4/2", "merge_index/start+2/8x4/3"),
            ("parameter_by_shape", "AMVR_signalled/steady+0/64x64/0", "AMVR_signalled/steady+0/64x64/3"),
            ("tu_geometry", "steady+2/p0/4x4", "tail+1/p0/4x4"),
            ("tu_geometry", "steady+2/p1/2x2", "tail+1/p1/2x2"),
            ("sao_enabled", "start+2/Y", "start+2/C"),
        )
        for group, allowed, excluded in cases:
            with self.subTest(group=group, excluded=excluded):
                self.assertIn(allowed, self.proof[group])
                self.assertNotIn(excluded, self.proof[group])

    def test_missing_event_stream_or_transform_plane_fails(self):
        h = "V H 0 90 0 2 7 0 8 8 0 0"
        use = "H 0 90 2 0 0 0 1 0 0 1"
        block = "V C 0 0 0 0 8 8 1"
        unit = "U CU 0 0 0 8 8 1"
        planes = ["V T 0 0 0 8 8 0 0", "V T 0 0 1 4 4 0 0", "V T 0 0 2 4 4 0 0"]
        for skip in (None, use, unit, *planes):
            trace = "\n".join(row for row in (h, use, block, unit, *planes) if row != skip) + "\n"
            result = subprocess.run(["awk", "-v", "stage=combined", "-f", HERE / "r28-tools.awk"],
                                    input=trace, capture_output=True, text=True)
            self.assertEqual(result.returncode == 0, skip is None, result.stderr)

    def test_sao_joins_at_poc_wrap_in_decode_order(self):
        fields = {"ph_pic_order_cnt_lsb": [254, 2, 0], "sps_log2_max_pic_order_cnt_lsb_minus4": [4],
                  "sps_sao_enabled_flag": [1], "sh_sao_luma_used_flag": [1, 1, 1],
                  "sh_sao_chroma_used_flag": [1, 1, 0]}
        order = {"0/IDR": 254, "1/start+4": 258, "2/start+2": 256}
        observed = tools.sao_choices(fields, order)
        self.assertIn("start+2/Y", observed)
        self.assertNotIn("start+2/C", observed)
        fields["sh_sao_chroma_used_flag"].pop()
        with self.assertRaises(ValueError):
            tools.sao_choices(fields, order)
        fields["ph_pic_order_cnt_lsb"] = [254, 0, 2]
        with self.assertRaises(ValueError):
            tools.sao_choices(fields, order)


if __name__ == "__main__":
    unittest.main()
