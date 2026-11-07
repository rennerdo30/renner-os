#!/usr/bin/env python3
"""renner.dev OS default wallpaper: dark, a fine dot grid, an orange glow top
left and a cyan glow bottom right (the installer and renner.dev look).

    python theme/wallpaper.py     # writes system/usr/share/wallpapers/renner/
"""
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent.parent / "system/usr/share/wallpapers/renner"
W, H = 3840, 2160


def glow(size, center, radius, color, strength):
    layer = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(layer)
    x, y = center
    d.ellipse((x - radius, y - radius, x + radius, y + radius),
              fill=tuple(int(c * strength) for c in color))
    return layer.filter(ImageFilter.GaussianBlur(radius * 0.55))


def main() -> None:
    base = Image.new("RGB", (W, H), (8, 9, 15))
    # Glows are added (screen-like) so they brighten the dark base softly.
    for layer in (glow((W, H), (int(W * 0.12), int(H * 0.05)), 1100, (255, 122, 26), 0.22),
                  glow((W, H), (int(W * 0.92), int(H * 0.98)), 1250, (45, 226, 240), 0.13)):
        base = ImageChops.add(base, layer)
    # Dot grid: 2 px dots every 48 px, faint, fading out towards the bottom.
    dots = ImageDraw.Draw(base)
    for y in range(24, H, 48):
        alpha = max(0.0, 1.0 - y / H * 0.9)
        shade = int(22 + 18 * alpha)
        for x in range(24, W, 48):
            dots.ellipse((x - 1, y - 1, x + 1, y + 1), fill=(shade, shade + 2, shade + 10))
    images = OUT / "contents/images"
    images.mkdir(parents=True, exist_ok=True)
    base.save(images / "3840x2160.png", optimize=True)
    (OUT / "metadata.json").write_text(json.dumps({
        "KPlugin": {"Id": "renner", "Name": "renner.dev", "License": "CC-BY-SA-4.0",
                    "Authors": [{"Name": "renner.dev"}]},
    }, indent=2) + "\n", newline="\n")
    print(images / "3840x2160.png")


if __name__ == "__main__":
    main()
