#!/usr/bin/env python3
"""Unit tests for scripts/generate_icons.py."""

import math
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import generate_icons  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SHEET = REPO / "assets" / "icons" / "cad-icons-hairline.svg"


def sheet(*symbols: str) -> str:
    return '<svg xmlns="http://www.w3.org/2000/svg">' + "".join(symbols) + "</svg>"


def symbol(name: str, *paths: str) -> str:
    return f'<symbol id="cad-{name}" viewBox="0 0 32 32"><g stroke-width="1.5">' + "".join(paths) + "</g></symbol>"


STROKE = "stroke:currentColor;fill:none"
FILL = "stroke:none;fill:currentColor"


class ParsePathTest(unittest.TestCase):
    def test_lines_absolute_relative_and_implicit(self):
        [(points, closed)] = generate_icons.parse_path("M1 2L3 4h2v-1 l1 1 1 1")
        self.assertEqual(points, [(1, 2), (3, 4), (5, 4), (5, 3), (6, 4), (7, 5)])
        self.assertFalse(closed)

    def test_implicit_lineto_after_moveto(self):
        [(points, _)] = generate_icons.parse_path("M0 0 10 0 10 10")
        self.assertEqual(points, [(0, 0), (10, 0), (10, 10)])

    def test_z_closes_and_splits_subpaths(self):
        subpaths = generate_icons.parse_path("M0 0H4V4Z M10 10H12")
        self.assertEqual(subpaths, [([(0, 0), (4, 0), (4, 4)], True), ([(10, 10), (12, 10)], False)])

    def test_path_ending_at_its_start_is_closed_without_the_repeat(self):
        [(points, closed)] = generate_icons.parse_path("M0 0H4V4H0V0")
        self.assertTrue(closed)
        self.assertEqual(points, [(0, 0), (4, 0), (4, 4), (0, 4)])

    def test_full_circle_from_two_arcs(self):
        [(points, closed)] = generate_icons.parse_path("M6 14a8 8 0 1 0 16 0a8 8 0 1 0 -16 0")
        self.assertTrue(closed)
        for x, y in points:
            self.assertAlmostEqual(math.hypot(x - 14, y - 14), 8, places=6)
        # Within tolerance of the true circle between points too.
        step = math.dist(points[0], points[1])
        self.assertLess(8 - math.sqrt(64 - (step / 2) ** 2), generate_icons.TOLERANCE)

    def test_arc_sweep_picks_the_side(self):
        [(clockwise, _)] = generate_icons.parse_path("M6 22A10 10 0 0 1 26 22")
        [(counter, _)] = generate_icons.parse_path("M6 22A10 10 0 0 0 26 22")
        self.assertLess(min(y for _, y in clockwise), 22)  # sweep 1 bulges up (y down)
        self.assertGreater(max(y for _, y in counter), 22)

    def test_cubic_and_smooth_cubic_end_on_their_endpoints(self):
        [(points, _)] = generate_icons.parse_path("M6 26C6 16 16 8 26 8S30 20 20 20")
        self.assertEqual(points[0], (6, 26))
        self.assertIn((26, 8), points)
        self.assertEqual(points[-1], (20, 20))

    def test_unsupported_command_fails(self):
        with self.assertRaises(generate_icons.IconError):
            generate_icons.parse_path("M0 0Q1 1 2 2")


class ParseIconsTest(unittest.TestCase):
    def test_reads_names_styles_and_opacity(self):
        _, [icon] = generate_icons.parse_icons(
            sheet(symbol("zoom-fit", f'<path d="M1 1H5" style="{STROKE};opacity:0.4"/>', f'<path d="M1 1H5V5Z" style="{FILL}"/>'))
        )
        self.assertEqual((icon.name, icon.enum_name, icon.stroke_width), ("zoom-fit", "ZoomFit", 1.5))
        stroke, fill = icon.polylines
        self.assertEqual((stroke.filled, stroke.closed, stroke.opacity), (False, False, 0.4))
        self.assertEqual((fill.filled, fill.closed, fill.opacity), (True, True, 1.0))

    def test_concave_fill_fails(self):
        with self.assertRaisesRegex(generate_icons.IconError, "convex"):
            generate_icons.parse_icons(sheet(symbol("x", f'<path d="M0 0H10V10H5V5H0Z" style="{FILL}"/>')))

    def test_bad_sheets_fail(self):
        cases = {
            "prefix": '<svg xmlns="http://www.w3.org/2000/svg"><symbol id="zoom" viewBox="0 0 32 32"/></svg>',
            "element": sheet(symbol("a", '<circle r="1"/>')),
            "duplicate": sheet(symbol("a-b", f'<path d="M0 0H1" style="{STROKE}"/>'), symbol("a-b", f'<path d="M0 0H1" style="{STROKE}"/>')),
            "no paths": sheet(symbol("a")),
            "neither": sheet(symbol("a", '<path d="M0 0H1" style="stroke:none;fill:none"/>')),
        }
        for name, svg in cases.items():
            with self.subTest(name), self.assertRaises(generate_icons.IconError):
                generate_icons.parse_icons(svg)


class SheetTest(unittest.TestCase):
    def test_the_committed_sheet_generates_a_header_that_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "generated" / "gui" / "icons.hpp"
            self.assertEqual(generate_icons.main(["generate_icons.py", str(SHEET), str(header)]), 0)
            text = header.read_text()
            for name in ("Delete", "ClearRulers", "Move", "Select"):
                self.assertIn(f"        {name},\n", text)
            cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
            if not cxx:
                self.skipTest("no C++ compiler")
            source = Path(tmp) / "check.cpp"
            source.write_text('#include "generated/gui/icons.hpp"\nstatic_assert(le::icon_data::entry(le::Icon::Move).polyline_count > 0);\n')
            subprocess.run([cxx, "-std=c++23", "-fsyntax-only", "-Werror", "-Wall", "-I", tmp, str(source)], check=True)


if __name__ == "__main__":
    unittest.main()
