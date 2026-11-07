#!/usr/bin/env python3
"""Copies the panel functions from theme/*.js (rennerSystray, rennerClock, ...)
into the global theme's layout script, below a marker, so new users get the
same panel that lab/plasma-eval.sh applies live.

    python theme/layout_sync.py
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAYOUT = HERE.parent / ("system/usr/share/plasma/look-and-feel/dev.renner.desktop/"
                        "contents/layouts/org.kde.plasma.desktop-layout.js")
MARKER = "// --- generated from theme/*.js by theme/layout_sync.py ---"
OLD_MARKER = "// --- generated from theme/systray.js ---"


def main() -> None:
    text = LAYOUT.read_text(encoding="utf-8")
    for marker in (MARKER, OLD_MARKER):
        if marker in text:
            text = text[:text.index(marker)]
    functions = "\n".join(p.read_text(encoding="utf-8") for p in sorted(HERE.glob("*.js")))
    LAYOUT.write_bytes((text.rstrip("\n") + "\n\n" + MARKER + "\n" + functions).encode("utf-8"))
    print(f"layout: {len(list(HERE.glob('*.js')))} function files")


if __name__ == "__main__":
    main()
