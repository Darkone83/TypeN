from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent

REQUIRED = {
    "PySide6": "PySide6",
    "serial": "pyserial",
    "esptool": "esptool",
}


def missing_packages() -> list[str]:
    missing: list[str] = []
    for module_name, package_name in REQUIRED.items():
        if importlib.util.find_spec(module_name) is None:
            missing.append(package_name)
    return missing


def ensure_dependencies() -> None:
    missing = missing_packages()
    if not missing:
        return

    req = ROOT / "requirements.txt"

    print("TypeN Flasher is missing required Python packages:")
    for package in missing:
        print(f"  - {package}")
    print()
    print("Installing dependencies for this Python environment...")

    subprocess.check_call(
        [
            sys.executable,
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "-r",
            str(req),
        ]
    )

    still_missing = missing_packages()
    if still_missing:
        raise RuntimeError(
            "Dependency installation completed but these modules are still "
            f"unavailable: {', '.join(still_missing)}"
        )


def main() -> int:
    ensure_dependencies()

    from app import main as app_main

    return int(app_main() or 0)


if __name__ == "__main__":
    raise SystemExit(main())
