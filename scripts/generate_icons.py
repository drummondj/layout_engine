#!/usr/bin/env python3
"""Generates the GUI's icon header from an SVG symbol sheet.

Each <symbol id="cad-NAME" viewBox="0 0 S S"> becomes `le::Icon::Name`. Its
paths are flattened into polylines in viewBox units (arcs and curves become
line segments) that `le::gui::draw_icon` strokes or fills with ImDrawList, so
the icons need no font or texture and stay sharp at any size. Supported:
<path> elements with M/L/H/V/C/S/A/Z commands (either case), the
`stroke-width` of the symbol's <g>, and `style` with `fill:currentColor`
(filled; each subpath must be convex) or `stroke:currentColor` (stroked) and
an optional `opacity`.

Usage: generate_icons.py <icons.svg> <output.hpp>
"""

import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

SVG_NS = "{http://www.w3.org/2000/svg}"
ID_PREFIX = "cad-"
# Maximum distance, in viewBox units, between a flattened curve and the true one.
TOLERANCE = 0.05


@dataclass
class Polyline:
    points: list[tuple[float, float]]
    closed: bool
    filled: bool
    opacity: float


@dataclass
class Icon:
    name: str  # without ID_PREFIX, e.g. "zoom-in"
    stroke_width: float
    polylines: list[Polyline] = field(default_factory=list)

    @property
    def enum_name(self) -> str:
        return "".join(part.capitalize() for part in self.name.split("-"))


class IconError(ValueError):
    pass


_TOKEN = re.compile(r"[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def _tokens(d: str) -> list[str]:
    return _TOKEN.findall(d)


def _arc_points(start, rx, ry, phi_deg, large_arc, sweep, end):
    """Points after `start` along an SVG elliptical arc (SVG 1.1 F.6.5)."""
    x1, y1 = start
    x2, y2 = end
    if (x1, y1) == (x2, y2):
        return []
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        return [end]
    phi = math.radians(phi_deg)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p = cos_phi * dx + sin_phi * dy
    y1p = -sin_phi * dx + cos_phi * dy
    scale = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den))
    if large_arc == sweep:
        coef = -coef
    cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
    cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2
    cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dtheta > 0:
        dtheta -= 2 * math.pi
    elif sweep and dtheta < 0:
        dtheta += 2 * math.pi
    r = max(rx, ry)
    step = 2 * math.acos(max(-1.0, 1 - TOLERANCE / r)) if r > TOLERANCE else math.pi
    segments = max(2, math.ceil(abs(dtheta) / step))
    points = []
    for i in range(1, segments + 1):
        t = theta1 + dtheta * i / segments
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        points.append((cos_phi * ex - sin_phi * ey + cx, sin_phi * ex + cos_phi * ey + cy))
    points[-1] = end
    return points


def _cubic_points(p0, p1, p2, p3):
    """Points after `p0` along a cubic Bezier."""
    # Second-difference bound on the flattening error.
    dd = max(
        math.hypot(p0[0] - 2 * p1[0] + p2[0], p0[1] - 2 * p1[1] + p2[1]),
        math.hypot(p1[0] - 2 * p2[0] + p3[0], p1[1] - 2 * p2[1] + p3[1]),
    )
    segments = max(2, math.ceil(math.sqrt(0.75 * dd / TOLERANCE)))
    points = []
    for i in range(1, segments + 1):
        t = i / segments
        u = 1 - t
        points.append(
            (
                u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1],
            )
        )
    points[-1] = p3
    return points


_ARG_COUNTS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "A": 7, "Z": 0}


def parse_path(d: str) -> list[tuple[list[tuple[float, float]], bool]]:
    """Subpaths of an SVG path as (points, closed). A subpath ending where it
    starts counts as closed, without the repeated point."""
    tokens = _tokens(d)
    subpaths: list[tuple[list[tuple[float, float]], bool]] = []
    points: list[tuple[float, float]] = []
    closed = False
    current = (0.0, 0.0)
    start = (0.0, 0.0)
    last_control = None  # second control point of the previous C/S, for S
    command = None
    i = 0

    def finish():
        nonlocal points, closed
        if len(points) > 1:
            if not closed and math.dist(points[0], points[-1]) < 1e-6:
                closed = True
            if closed and math.dist(points[0], points[-1]) < 1e-6:
                points.pop()
            subpaths.append((points, closed))
        points, closed = [], False

    while i < len(tokens):
        token = tokens[i]
        if token.isalpha():
            command = token
            i += 1
            upper = command.upper()
            if upper not in _ARG_COUNTS:
                raise IconError(f"unsupported path command {command!r} in {d!r}")
            if upper == "Z":
                closed = True
                current = start
                finish()
                points = [start]  # drawing after Z without a moveto starts here
                last_control = None
                continue
        elif command is None:
            raise IconError(f"path data doesn't start with a command: {d!r}")
        upper = command.upper()
        relative = command.islower()
        count = _ARG_COUNTS[upper]
        if upper == "Z" or i + count > len(tokens):
            raise IconError(f"malformed path data: {d!r}")
        args = [float(t) for t in tokens[i : i + count]]
        i += count
        ox, oy = current if relative else (0.0, 0.0)
        control = None
        if upper == "M":
            finish()
            current = (args[0] + ox, args[1] + oy)
            start = current
            points = [current]
            # Coordinates after a moveto are implicit linetos.
            command = "l" if relative else "L"
        elif upper == "L":
            current = (args[0] + ox, args[1] + oy)
            points.append(current)
        elif upper == "H":
            current = (args[0] + ox, current[1])
            points.append(current)
        elif upper == "V":
            current = (current[0], args[0] + (current[1] if relative else 0.0))
            points.append(current)
        elif upper in ("C", "S"):
            if upper == "C":
                c1 = (args[0] + ox, args[1] + oy)
                rest = args[2:]
            else:
                c1 = (2 * current[0] - last_control[0], 2 * current[1] - last_control[1]) if last_control else current
                rest = args
            c2 = (rest[0] + ox, rest[1] + oy)
            end = (rest[2] + ox, rest[3] + oy)
            points.extend(_cubic_points(current, c1, c2, end))
            current = end
            control = c2
        elif upper == "A":
            end = (args[5] + ox, args[6] + oy)
            points.extend(_arc_points(current, args[0], args[1], args[2], bool(args[3]), bool(args[4]), end))
            current = end
        last_control = control
    finish()
    return subpaths


def _is_convex(points) -> bool:
    sign = 0
    n = len(points)
    for k in range(n):
        ax, ay = points[k]
        bx, by = points[(k + 1) % n]
        cx, cy = points[(k + 2) % n]
        cross = (bx - ax) * (cy - by) - (by - ay) * (cx - bx)
        if abs(cross) < 1e-9:
            continue
        if sign == 0:
            sign = 1 if cross > 0 else -1
        elif (cross > 0) != (sign > 0):
            return False
    return True


def _style(element) -> dict[str, str]:
    style = {}
    for item in element.get("style", "").split(";"):
        if ":" in item:
            key, value = item.split(":", 1)
            style[key.strip()] = value.strip()
    return style


def parse_icons(svg_text: str) -> tuple[float, list[Icon]]:
    """(viewBox size, icons in sheet order)."""
    root = ET.fromstring(svg_text)
    icons: list[Icon] = []
    view_size = None
    for symbol in root.iter(f"{SVG_NS}symbol"):
        symbol_id = symbol.get("id", "")
        if not symbol_id.startswith(ID_PREFIX):
            raise IconError(f"symbol id {symbol_id!r} doesn't start with {ID_PREFIX!r}")
        name = symbol_id[len(ID_PREFIX) :]
        view_box = [float(v) for v in symbol.get("viewBox", "").split()]
        if len(view_box) != 4 or view_box[0] != 0 or view_box[1] != 0 or view_box[2] != view_box[3]:
            raise IconError(f"{symbol_id}: viewBox must be '0 0 S S'")
        if view_size is None:
            view_size = view_box[2]
        elif view_box[2] != view_size:
            raise IconError(f"{symbol_id}: viewBox size differs from the other symbols'")
        stroke_width = 1.0
        for g in symbol.iter(f"{SVG_NS}g"):
            if g.get("stroke-width"):
                stroke_width = float(g.get("stroke-width"))
        icon = Icon(name, stroke_width)
        for element in symbol.iter():
            tag = element.tag.removeprefix(SVG_NS)
            if tag in ("symbol", "g"):
                continue
            if tag != "path":
                raise IconError(f"{symbol_id}: unsupported element <{tag}>")
            style = _style(element)
            filled = style.get("fill", "none") != "none"
            stroked = style.get("stroke", "none") != "none"
            if filled == stroked:
                raise IconError(f"{symbol_id}: a path must be either filled or stroked")
            opacity = float(style.get("opacity", "1"))
            for points, closed in parse_path(element.get("d", "")):
                if filled and not _is_convex(points):
                    raise IconError(f"{symbol_id}: filled subpaths must be convex")
                icon.polylines.append(Polyline(points, closed or filled, filled, opacity))
        if not icon.polylines:
            raise IconError(f"{symbol_id}: no paths")
        icons.append(icon)
    if not icons:
        raise IconError("no <symbol> elements")
    names = [icon.enum_name for icon in icons]
    duplicates = sorted({n for n in names if names.count(n) > 1})
    if duplicates:
        raise IconError(f"duplicate icon names: {', '.join(duplicates)}")
    return view_size, icons


def _f(value: float) -> str:
    text = f"{value:.3f}".rstrip("0")
    if text in ("-0.", "0."):
        text = "0."
    return text + "0f" if text.endswith(".") else text + "f"


def render_header(view_size: float, icons: list[Icon], source_name: str) -> str:
    lines = [
        f"// Generated by scripts/generate_icons.py from {source_name}. Do not edit.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace le",
        "{",
        "    /// @brief The GUI's icon set; draw one with le::gui::draw_icon (or le::ext::icon).",
        "    enum class Icon : uint16_t",
        "    {",
    ]
    lines += [f"        {icon.enum_name}," for icon in icons]
    lines += [
        "    };",
        "",
        f"    inline constexpr int kIconCount = {len(icons)};",
        "}",
        "",
        "namespace le::icon_data",
        "{",
        "    struct Point",
        "    {",
        "        float x;",
        "        float y;",
        "    };",
        "",
        "    /// One stroked or filled run of points, in viewBox units.",
        "    struct Polyline",
        "    {",
        "        uint32_t first_point;",
        "        uint16_t point_count;",
        "        bool closed;",
        "        bool filled; // convex",
        "        float opacity;",
        "    };",
        "",
        "    struct IconEntry",
        "    {",
        "        std::string_view name; // as in the SVG, without its \"" + ID_PREFIX + "\" prefix",
        "        uint16_t first_polyline;",
        "        uint16_t polyline_count;",
        "        float stroke_width; // viewBox units",
        "    };",
        "",
        f"    inline constexpr float kViewBoxSize = {_f(view_size)};",
        "",
        "    inline constexpr Point kPoints[] = {",
    ]
    polyline_rows = []
    icon_rows = []
    point_index = 0
    for icon in icons:
        icon_rows.append(f'        {{"{icon.name}", {len(polyline_rows)}, {len(icon.polylines)}, {_f(icon.stroke_width)}}},')
        for polyline in icon.polylines:
            coords = ", ".join(f"{{{_f(x)}, {_f(y)}}}" for x, y in polyline.points)
            lines.append(f"        {coords},")
            polyline_rows.append(
                f"        {{{point_index}, {len(polyline.points)}, {str(polyline.closed).lower()}, "
                f"{str(polyline.filled).lower()}, {_f(polyline.opacity)}}},"
            )
            point_index += len(polyline.points)
    lines += ["    };", "", "    inline constexpr Polyline kPolylines[] = {"]
    lines += polyline_rows
    lines += ["    };", "", "    inline constexpr IconEntry kIcons[] = {"]
    lines += icon_rows
    lines += [
        "    };",
        "    static_assert(sizeof(kIcons) / sizeof(kIcons[0]) == kIconCount);",
        "",
        "    /// @brief `icon`'s entry.",
        "    constexpr const IconEntry &entry(Icon icon) { return kIcons[static_cast<uint16_t>(icon)]; }",
        "}",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    source, output = Path(argv[1]), Path(argv[2])
    try:
        view_size, icons = parse_icons(source.read_text())
    except (IconError, ET.ParseError) as error:
        print(f"generate_icons: {source}: {error}", file=sys.stderr)
        return 1
    header = render_header(view_size, icons, source.name)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != header:
        output.write_text(header)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
