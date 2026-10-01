from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create a portable TypeN full-flash package from an ESP-IDF "
            "build directory while preserving the filenames and paths from "
            "flasher_args.json."
        )
    )
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()

    build = args.build_dir.resolve()
    output = args.output_dir.resolve()

    flasher = build / "flasher_args.json"
    if not flasher.is_file():
        raise SystemExit(f"Missing: {flasher}")

    data = json.loads(flasher.read_text(encoding="utf-8"))
    flash_files = data.get("flash_files")

    if not isinstance(flash_files, dict) or not flash_files:
        raise SystemExit("flasher_args.json does not contain flash_files")

    output.mkdir(parents=True, exist_ok=True)

    shutil.copy2(flasher, output / "flasher_args.json")

    copied: set[str] = set()

    for relative_name in flash_files.values():
        rel = str(relative_name).replace("\\", "/")
        if rel in copied:
            continue

        source = build / Path(rel)
        destination = output / Path(rel)

        if not source.is_file():
            raise SystemExit(
                "Required build artifact is missing:\n"
                f"  {source}"
            )

        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied.add(rel)

    print(f"Created firmware package: {output}")
    for rel in sorted(copied):
        print(f"  {rel}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
