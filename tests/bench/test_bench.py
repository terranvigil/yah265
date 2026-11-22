#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""y265 bench: the matched-size search, the scoring rules and the verdict.

The CIF test encodes with the real binary (BIN, default build/y265, then
build-rel/y265) and skips when it or the clip (CIF, default
tests/corpus/foreman_cif.y4m) is missing."""
import importlib.util, math, os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location(
    "y265_bench", os.path.join(ROOT, "scripts", "y265_bench.py"))
yb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(yb)
yb.log = lambda *_: None


def curve(q, base=1e6, slope=-0.12):
    return int(base * math.exp(slope * q))


class Search(unittest.TestCase):
    def test_smooth_curve_lands_inside_half_percent(self):
        target = curve(26.37)
        best, pts, _ = yb.secant(curve, target, 25)
        self.assertLess(best[2], 0.005)
        self.assertLessEqual(len(pts), 4)

    def test_staircase_moves_x265_onto_our_point(self):
        # sizes only change at whole CRF steps: no CRF lands within 1%
        ours = lambda q: curve(math.floor(q))
        ref = lambda q: curve(q - 3.5, base=1.0e6)
        m = yb.match(ours, ref, 28.0)
        self.assertTrue(m["moved_ref"])
        self.assertTrue(m["matched"], m)
        self.assertNotEqual(m["ref_crf"], 28.0)

    def test_unreachable_target_is_reported_unmatched(self):
        ours = lambda q: 1000 if q > 30 else 5000
        ref = lambda q: 3000
        m = yb.match(ours, ref, 28.0)
        self.assertFalse(m["matched"])
        self.assertEqual(m["ref_bytes"], 3000)


class Rules(unittest.TestCase):
    def test_v1_model_by_size_and_rate(self):
        self.assertTrue(yb.v1_model("M", 1920, 1080, 30).endswith(
            "vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json"))
        self.assertTrue(yb.v1_model("M", 2560, 1440, 24).endswith(
            "vmaf_v1.0.16_1d5h_2160.json"))
        self.assertTrue(yb.v1_model("M", 3840, 2160, 60).endswith(
            "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_1d5h_2160.json"))
        self.assertTrue(yb.v1_model("M", 1080, 1920, 30).endswith(
            "_3d0h.json"))

    def test_display_upscale(self):
        self.assertTrue(yb.upscaled(352, 288))
        self.assertTrue(yb.upscaled(1280, 720))
        self.assertFalse(yb.upscaled(1920, 1080))
        self.assertFalse(yb.upscaled(1920, 800))

    def test_window_centres_and_whole(self):
        info = {"dur": 100.0, "fps": 25.0}
        self.assertEqual(yb.window(info, 20, None, False), (40.0, 500))
        self.assertEqual(yb.window(info, 20, 5, False), (5, 500))
        self.assertEqual(yb.window(info, 20, None, True), (0.0, None))
        self.assertEqual(yb.window({"dur": 8.0, "fps": 25.0}, 20, None,
                                   False), (0.0, None))


class Verdict(unittest.TestCase):
    def test_win(self):
        v = yb.verdict(2.31, 1.3, 0.003, True)
        self.assertIn("2.31x as fast as x265", v)
        self.assertIn("1.30 VMAF v1 points ahead", v)
        self.assertIn("same file size (+0.30%)", v)

    def test_loss_says_so(self):
        v = yb.verdict(0.85, -0.4, -0.002, True)
        self.assertIn("slower than x265 (0.85x its speed)", v)
        self.assertIn("0.40 VMAF v1 points behind", v)

    def test_unmatched_and_busy(self):
        v = yb.verdict(1.2, 0.5, 0.04, False, (9.0, 8))
        self.assertIn("sizes didn't match", v)
        self.assertIn("speed number is rough", v)


def find_bin():
    for c in (os.environ.get("BIN"), os.path.join(ROOT, "build", "y265"),
              os.path.join(ROOT, "build-rel", "y265")):
        if c and os.access(c, os.X_OK):
            return c
    return None


class CifSolve(unittest.TestCase):
    """The search with real encodes: a target from a fractional CRF that the
    search doesn't start on, solved within 1%."""

    def test_foreman_matches_within_one_percent(self):
        b = find_bin()
        clip = os.environ.get("CIF", os.path.join(ROOT, "tests", "corpus",
                                                  "foreman_cif.y4m"))
        if not b or not os.path.exists(clip):
            self.skipTest("needs the y265 binary and foreman_cif.y4m")
        wd = tempfile.mkdtemp(prefix="benchtest-")
        t = {"y265": b}

        def size(q):
            out = os.path.join(wd, "o.hevc")
            cmd = yb.ours_cmd(t, clip, q, 1, out)
            cmd[2:2] = ["--frames", "30"]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            n = os.path.getsize(out)
            os.remove(out)
            return n

        target = size(30.4)
        best, pts, _ = yb.secant(size, target, 25.0)
        os.rmdir(wd)
        self.assertLess(best[2], yb.TOL, (best, pts))
        self.assertLessEqual(len(pts), 8)


if __name__ == "__main__":
    unittest.main(verbosity=2)
