#!/usr/bin/env python3
"""renner.dev Plasma style: the panel, start menu, tray popups, tooltips and
list highlights, generated as Plasma "FrameSvg" nine-part frames.

Each frame is drawn as separate pieces (topleft, top, ..., center), plus mask
pieces for the blur behind translucent popups, shadow pieces, and margin hints.
Files we do not write fall back to Breeze (Plasma does that by itself).

    python theme/plasma_style.py     # writes system/usr/share/plasma/desktoptheme/renner/
"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "system/usr/share/plasma/desktoptheme/renner"

SIDES = ["topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright"]


class Svg:
    def __init__(self):
        self.parts = []
        self.y = 0          # pieces are stacked in rows so none overlap

    def rect(self, id_, x, y, w, h, fill, opacity=1.0):
        self.parts.append(f'<rect id="{id_}" x="{x}" y="{y}" width="{w}" height="{h}" '
                          f'fill="{fill}" fill-opacity="{opacity:.3f}"/>')

    def group(self, id_, body):
        self.parts.append(f'<g id="{id_}">{body}</g>')

    def text(self, w, h):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
                f'viewBox="0 0 {w} {h}">\n' + "\n".join(self.parts) + "\n</svg>\n")


def frame(svg, prefix, r, fill, alpha, border, border_alpha, edges=("top", "bottom", "left", "right"), y0=0):
    """A rounded frame of corner radius r with a 1 px border, as nine pieces.

    Pieces sit side by side in one row starting at y0; returns the row height.
    edges: which sides carry the border (a full-width panel only has "top").
    """
    p = f"{prefix}-" if prefix else ""
    r = max(r, 1)
    size = {"topleft": (r, r), "top": (1, r), "topright": (r, r), "left": (r, 1), "center": (1, 1),
            "right": (r, 1), "bottomleft": (r, r), "bottom": (1, r), "bottomright": (r, r)}
    x = 0
    for side in SIDES:
        w, h = size[side]
        ox, oy = x, y0
        body = []
        corner = side in ("topleft", "topright", "bottomleft", "bottomright")
        if corner:
            # Quarter disc of radius r, centred on the inner corner.
            cx = ox + (r if "left" in side else 0)
            cy = oy + (r if "top" in side else 0)
            sx = -1 if "left" in side else 1
            sy = -1 if "top" in side else 1
            vert = "top" if "top" in side else "bottom"
            horiz = "left" if "left" in side else "right"
            has_border = vert in edges and horiz in edges
            if vert in edges and horiz in edges:
                d = (f"M{cx},{cy} L{cx + sx * r},{cy} A{r},{r} 0 0 {1 if sx * sy > 0 else 0} "
                     f"{cx},{cy + sy * r} Z")
            else:
                d = f"M{ox},{oy} h{w} v{h} h{-w} Z"
            body.append(f'<path d="{d}" fill="{fill}" fill-opacity="{alpha:.3f}"/>')
            if has_border:
                rr = r - 0.5
                arc = (f"M{cx + sx * rr},{cy} A{rr},{rr} 0 0 {1 if sx * sy > 0 else 0} {cx},{cy + sy * rr}")
                body.append(f'<path d="{arc}" fill="none" stroke="{border}" stroke-opacity="{border_alpha:.3f}" stroke-width="1"/>')
            else:
                # Square corner: border only on the sides that have one.
                if vert in edges:
                    by = oy if vert == "top" else oy + h - 1
                    body.append(f'<rect x="{ox}" y="{by}" width="{w}" height="1" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
                if horiz in edges:
                    bx = ox if horiz == "left" else ox + w - 1
                    body.append(f'<rect x="{bx}" y="{oy}" width="1" height="{h}" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
        else:
            body.append(f'<rect x="{ox}" y="{oy}" width="{w}" height="{h}" fill="{fill}" fill-opacity="{alpha:.3f}"/>')
            if side in edges:
                if side == "top":
                    body.append(f'<rect x="{ox}" y="{oy}" width="{w}" height="1" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
                elif side == "bottom":
                    body.append(f'<rect x="{ox}" y="{oy + h - 1}" width="{w}" height="1" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
                elif side == "left":
                    body.append(f'<rect x="{ox}" y="{oy}" width="1" height="{h}" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
                elif side == "right":
                    body.append(f'<rect x="{ox + w - 1}" y="{oy}" width="1" height="{h}" fill="{border}" fill-opacity="{border_alpha:.3f}"/>')
        # The bounding box must be exactly the piece: an invisible rect fixes it.
        body.insert(0, f'<rect x="{ox}" y="{oy}" width="{w}" height="{h}" fill="#000" fill-opacity="0"/>')
        svg.group(f"{p}{side}", "".join(body))
        x += w + 2
    return r + 2


def mask(svg, r, edges, y0):
    """mask-*: the same shape, opaque: where KWin blurs behind a popup."""
    return frame(svg, "mask", r, "#000000", 1.0, "#000000", 0.0, edges, y0)


def shadow(svg, size, y0, strength=0.45):
    """shadow-*: a soft drop shadow around the frame (KWin draws it)."""
    x = 0
    for side in SIDES:
        w = size if "left" in side or "right" in side else 1
        h = size if "top" in side or "bottom" in side else 1
        gid = f"g-{side}"
        if side == "center":
            body = f'<rect x="{x}" y="{y0}" width="1" height="1" fill="#000" fill-opacity="0"/>'
        elif side in ("top", "bottom", "left", "right"):
            # Opaque next to the frame, fading outwards.
            x1, y1, x2, y2 = {"top": (0, 1, 0, 0), "bottom": (0, 0, 0, 1),
                              "left": (1, 0, 0, 0), "right": (0, 0, 1, 0)}[side]
            body = (f'<defs><linearGradient id="{gid}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
                    f'<stop offset="0" stop-color="#000" stop-opacity="{strength}"/>'
                    f'<stop offset="1" stop-color="#000" stop-opacity="0"/></linearGradient></defs>'
                    f'<rect x="{x}" y="{y0}" width="{w}" height="{h}" fill="url(#{gid})"/>')
        else:
            cx = x + (size if "left" in side else 0)
            cy = y0 + (size if "top" in side else 0)
            body = (f'<defs><radialGradient id="{gid}" cx="{cx}" cy="{cy}" r="{size}" gradientUnits="userSpaceOnUse">'
                    f'<stop offset="0" stop-color="#000" stop-opacity="{strength}"/>'
                    f'<stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient></defs>'
                    f'<rect x="{x}" y="{y0}" width="{w}" height="{h}" fill="url(#{gid})"/>')
        svg.group(f"shadow-{side}", body)
        x += w + 2
    return size + 2


def hints(svg, prefix, margins, y0):
    """hint-*-margin rects: their width/height is the content margin."""
    p = f"{prefix}-" if prefix else ""
    x = 0
    for name, value in margins.items():
        w = value if name in ("left", "right") else 1
        h = value if name in ("top", "bottom") else 1
        svg.rect(f"{p}hint-{name}-margin", x, y0, w, h, "#000", 0.0)
        x += w + 2
    return max(margins.values()) + 2


def write(path, svg, height):
    target = OUT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(svg.text(400, height), newline="\n")


SURFACE, RAISED, HOVER = "#11131d", "#11131d", "#181b28"
PANEL = "#0b0c14"
WHITE, ACCENT = "#ffffff", "#ff7a1a"
ALL = ("top", "bottom", "left", "right")


def popup(path, alpha):
    svg = Svg()
    y = frame(svg, "", 14, RAISED, alpha, WHITE, 0.10, ALL, 0)
    y += mask(svg, 14, ALL, y)
    y += shadow(svg, 28, y)
    y += hints(svg, "", {"top": 10, "bottom": 10, "left": 10, "right": 10}, y)
    y += hints(svg, "shadow", {"top": 22, "bottom": 30, "left": 26, "right": 26}, y)
    write(path, svg, y + 4)


def panel(path, alpha):
    svg = Svg()
    # Full width at the bottom: only a fine top line; floating panels get the
    # same frame with corners, which Plasma picks when borders are enabled.
    y = frame(svg, "", 1, PANEL, alpha, WHITE, 0.08, ("top",), 0)
    y += mask(svg, 1, ("top",), y)
    # Air around the icons, like the Windows 11 taskbar (owner, 2026-11-08).
    y += hints(svg, "", {"top": 10, "bottom": 10, "left": 8, "right": 8}, y)
    svg.rect("thick-center", 0, y, 1, 1, PANEL, alpha)
    write(path, svg, y + 4)


def tooltip(path):
    svg = Svg()
    y = frame(svg, "", 10, HOVER, 0.97, WHITE, 0.12, ALL, 0)
    y += mask(svg, 10, ALL, y)
    y += shadow(svg, 18, y, 0.35)
    y += hints(svg, "", {"top": 8, "bottom": 8, "left": 10, "right": 10}, y)
    y += hints(svg, "shadow", {"top": 14, "bottom": 20, "left": 16, "right": 16}, y)
    write(path, svg, y + 4)


def viewitem(path):
    """List and grid highlights: hover, selected, selected+hover (no normal)."""
    svg = Svg()
    y = 0
    for state, fill, alpha, border, balpha in [
        ("normal", WHITE, 0.0, WHITE, 0.0),
        ("hover", WHITE, 0.06, WHITE, 0.10),
        ("selected", ACCENT, 0.16, ACCENT, 0.55),
        ("selected+hover", ACCENT, 0.24, ACCENT, 0.70),
    ]:
        y += frame(svg, state, 9, fill, alpha, border, balpha, ALL, y)
    svg.rect("hint-tile-center", 0, y, 1, 1, "#000", 0.0)
    write(path, svg, y + 4)


def main() -> None:
    popup("dialogs/background.svg", 0.94)
    popup("translucent/dialogs/background.svg", 0.80)
    popup("opaque/dialogs/background.svg", 1.0)
    panel("widgets/panel-background.svg", 0.92)
    panel("translucent/widgets/panel-background.svg", 0.78)
    panel("opaque/widgets/panel-background.svg", 1.0)
    tooltip("widgets/tooltip.svg")
    viewitem("widgets/viewitem.svg")
    (OUT / "metadata.json").write_text(json.dumps({
        "KPackageStructure": "Plasma/Theme",
        "KPlugin": {"Id": "renner", "Name": "renner.dev", "Description": "renner.dev OS Plasma style",
                    "License": "GPL-2.0-or-later", "Version": "0.1",
                    "Authors": [{"Name": "renner.dev"}]},
        "X-Plasma-API": "5.0",
    }, indent=2) + "\n", newline="\n")
    (OUT / "plasmarc").write_text("[AdaptiveTransparency]\nenabled=true\n\n[ContrastEffect]\nenabled=true\nintensity=0.9\nsaturation=1.4\ncontrast=0.2\n", newline="\n")
    files = sorted(str(p.relative_to(OUT.parent.parent.parent.parent.parent)).replace("\\", "/") for p in OUT.rglob("*") if p.is_file())
    print("\n".join(files))


if __name__ == "__main__":
    main()
