#!/usr/bin/env python3

import argparse
import re
import shutil
from pathlib import Path


def contradiction_for(path: Path) -> str:
    name = path.name
    if "-ex1" in name or "example1" in name:
        return "l : ( FG ( NOT (on1)))"
    if "-ex2" in name or "example2" in name or "charger" in name.lower():
        return "g : ( FG ( NOT (chargedOnce)))"
    if "-ex3" in name or "example3" in name or "corridor" in name.lower():
        first_line = path.read_text().splitlines()[0]
        matches = [int(m.group(1)) for m in re.finditer(r"\brb(\d+)\b", first_line)]
        if not matches:
            raise ValueError(f"Cannot infer final rb-index for {path}")
        return f"l : ( FG ( NOT (rb{max(matches)})))"
    raise ValueError(f"Cannot infer benchmark family for {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a conflicting benchmark suite by appending one contradictory value per instance.")
    parser.add_argument("--in-dir", required=True, help="Directory containing *.input-only.txt cases.")
    parser.add_argument("--out-dir", required=True, help="Directory where conflicting instances will be written.")
    args = parser.parse_args()

    in_dir = Path(args.in_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(in_dir.glob("*.input-only.txt"))
    for src in files:
        text = src.read_text()
        if text and not text.endswith("\n"):
            text += "\n"
        text += contradiction_for(src) + "\n"
        (out_dir / src.name).write_text(text)

    manifest = in_dir / "manifest.txt"
    if manifest.exists():
        shutil.copy2(manifest, out_dir / "manifest.txt")


if __name__ == "__main__":
    main()
