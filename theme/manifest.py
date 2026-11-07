#!/usr/bin/env python3
"""Writes system/MANIFEST: every file under system/ that install.sh copies onto
the new system, one path per line. install.sh fetches the list first, then
each file, so adding a file to system/ is enough.

    python theme/manifest.py           # rewrite the list
    python theme/manifest.py --check   # exit 1 if the list is out of date
"""
import sys
from pathlib import Path

SYSTEM = Path(__file__).resolve().parent.parent / "system"


def wanted() -> str:
    files = sorted(p.relative_to(SYSTEM).as_posix() for p in SYSTEM.rglob("*")
                   if p.is_file() and p.name != "MANIFEST" and "__pycache__" not in p.parts)
    return "\n".join(files) + "\n"


def main() -> None:
    target = SYSTEM / "MANIFEST"
    text = wanted()
    if "--check" in sys.argv:
        current = target.read_text(encoding="utf-8") if target.exists() else ""
        if current != text:
            sys.exit("system/MANIFEST is out of date; run: python theme/manifest.py")
        return
    target.write_text(text, encoding="utf-8", newline="\n")
    print(f"{len(text.splitlines())} files in system/MANIFEST")


if __name__ == "__main__":
    main()
