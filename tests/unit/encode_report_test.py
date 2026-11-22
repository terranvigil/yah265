#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/encode_report.py on a canned --frame-stats file: no encoder,
no ffmpeg (thumbnails off), only the page it writes."""

import json
import os
import re
import sys
import tempfile
import unittest
from html.parser import HTMLParser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import encode_report as er  # noqa: E402

FRAMES = os.path.join(ROOT, "tests", "data", "report_frames.jsonl")
CONFIG = os.path.join(ROOT, "tests", "data", "report_config.txt")


class Strict(HTMLParser):
    """Counts tags and fails on anything that loads from elsewhere."""

    def __init__(self):
        super().__init__()
        self.tags = {}
        self.external = []

    def handle_starttag(self, tag, attrs):
        self.tags[tag] = self.tags.get(tag, 0) + 1
        for k, v in attrs:
            if k in ("src", "href") and v and not v.startswith("data:"):
                self.external.append(v)


class ReportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frames = er.load_frames(FRAMES)
        cls.cfg = er.load_config(CONFIG)
        cls.page = er.render(cls.frames, cls.cfg, "canned <clip>")

    def test_frames_sorted_to_display_order(self):
        self.assertEqual([r["frame"] for r in self.frames],
                         list(range(60)))

    def test_config_facts(self):
        f = er.config_facts(self.cfg)
        self.assertEqual((f["w"], f["h"]), (352, 288))
        self.assertAlmostEqual(f["fps"], 30.0)
        self.assertEqual(f["bufsize"], 1000.0)
        self.assertEqual(f["maxrate"], 500.0)
        f = er.config_facts(er.load_config(None))
        self.assertIsNone(f["fps"])

    def test_shots_split_at_the_cut(self):
        shots = er.build_shots(self.frames, 30.0)
        self.assertEqual([(s["first"], s["last"]) for s in shots],
                         [(0, 29), (30, 59)])
        self.assertEqual(shots[1]["why"], "scene")
        self.assertIn("screen", shots[0]["det"])
        self.assertIn("dark", shots[1]["det"])
        self.assertIn("jitter", shots[1]["det"])
        self.assertNotIn("screen", shots[1]["det"])

    def test_old_stats_fall_back_to_intra_pictures(self):
        old = [{k: r[k] for k in ("frame", "type", "qp", "bytes")}
               for r in self.frames]
        self.assertFalse(er.has_detections(old))
        self.assertEqual(er.shot_starts(old), [0, 30])
        page = er.render(old, [], "old")
        self.assertIn("written before", page)

    def test_runs(self):
        self.assertEqual(er.runs([0, 1, 1, 0, 1]), [(1, 3), (4, 5)])
        self.assertEqual(er.runs([]), [])

    def test_rolling_rate(self):
        k = er.rolling_kbps([{"bytes": 1000}] * 60, 30.0)
        self.assertAlmostEqual(k[0], 240.0)
        self.assertAlmostEqual(k[-1], 240.0)

    def test_bucket_keeps_the_mean(self):
        b = er.bucket(list(range(10000)), 100)
        self.assertEqual(len(b), 100)
        self.assertAlmostEqual(sum(v for _, v in b) / 100, 4999.5)

    def test_page_is_self_contained(self):
        p = Strict()
        p.feed(self.page)
        self.assertEqual(p.external, [])
        self.assertNotIn("http://", self.page)
        self.assertNotIn("https://", self.page)
        self.assertNotIn("@import", self.page)

    def test_page_sections(self):
        for s in ("Bitrate", "QP and frame types", "Buffer fill",
                  "What the encoder detected", "Shots", "Configuration",
                  "Screen content", "Handheld jitter", "Scene cuts",
                  "Dark", "N265_SCREEN"):
            self.assertIn(s, self.page)
        self.assertIn("canned &lt;clip&gt;", self.page)
        self.assertIn("prefers-color-scheme:dark", self.page)
        self.assertIn('name="viewport"', self.page)
        self.assertIn("Lowest", self.page)

    def test_embedded_data(self):
        m = re.search(r'<script type="application/json" id="data">(.*?)'
                      r'</script>', self.page, re.S)
        d = json.loads(m.group(1))
        self.assertEqual(len(d["f"]), 60)
        self.assertEqual(d["f"][30][1], "I")
        self.assertIn("scene cut", d["f"][30][5])
        self.assertIn("screen", d["f"][0][5])
        self.assertIsNotNone(d["f"][0][4])

    def test_cli_writes_the_page(self):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, "r.html")
            rc = er.main(["--frames", FRAMES, "--config", CONFIG,
                          "--no-thumbs", "-o", out])
            self.assertEqual(rc, 0)
            with open(out) as f:
                self.assertIn("<svg", f.read())

    def test_no_vbv_no_buffer_chart(self):
        rows = [dict(r) for r in self.frames]
        for r in rows:
            r.pop("vbv_fill")
        page = er.render(rows, self.cfg, "x")
        self.assertNotIn("Buffer fill", page)


if __name__ == "__main__":
    unittest.main()
