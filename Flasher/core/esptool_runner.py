from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
import subprocess
import sys
from typing import Callable

from .firmware_package import FirmwarePackage


LineCallback = Callable[[str], None]


class EsptoolRunner:
    def __init__(self, line_callback: LineCallback | None = None):
        self.line_callback = line_callback or (lambda _line: None)

    @staticmethod
    def _major_version() -> int:
        try:
            return int(version("esptool").split(".", 1)[0])
        except (PackageNotFoundError, ValueError):
            return 5

    @classmethod
    def _command_name(cls, modern: str, legacy: str) -> str:
        return modern if cls._major_version() >= 5 else legacy

    @classmethod
    def _normalize_arg(cls, value: str) -> str:
        if cls._major_version() < 5:
            return value

        if value.startswith("--"):
            return value.replace("_", "-")

        replacements = {
            "default_reset": "default-reset",
            "hard_reset": "hard-reset",
            "no_reset": "no-reset",
            "no_reset_no_sync": "no-reset-no-sync",
        }
        return replacements.get(value, value)

    def _run(self, args: list[str]) -> int:
        cmd = [sys.executable, "-m", "esptool", *args]
        self.line_callback("$ " + " ".join(f'"{x}"' if " " in x else x for x in cmd))

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

        assert proc.stdout is not None
        for line in proc.stdout:
            self.line_callback(line.rstrip())

        return proc.wait()

    def _base(
        self,
        *,
        chip: str,
        port: str,
        baud: int,
        before: str = "default_reset",
        after: str = "hard_reset",
    ) -> list[str]:
        return [
            "--chip",
            chip,
            "--port",
            port,
            "--baud",
            str(baud),
            "--before",
            self._normalize_arg(before),
            "--after",
            self._normalize_arg(after),
        ]

    def identify(self, *, chip: str, port: str, baud: int) -> int:
        command = self._command_name("flash-id", "flash_id")
        return self._run(
            self._base(chip=chip, port=port, baud=baud)
            + [command]
        )

    def erase(self, *, chip: str, port: str, baud: int) -> int:
        command = self._command_name("erase-flash", "erase_flash")
        return self._run(
            self._base(chip=chip, port=port, baud=baud)
            + [command]
        )

    def dump(
        self,
        *,
        chip: str,
        port: str,
        baud: int,
        output: str | Path,
    ) -> int:
        command = self._command_name("read-flash", "read_flash")
        return self._run(
            self._base(chip=chip, port=port, baud=baud)
            + [command, "0", "ALL", str(Path(output))]
        )

    def full_flash(
        self,
        *,
        package: FirmwarePackage,
        port: str,
        baud: int,
    ) -> int:
        extra = package.args.get("extra_esptool_args") or {}
        before = str(extra.get("before") or "default_reset")
        after = str(extra.get("after") or "hard_reset")

        command = self._command_name("write-flash", "write_flash")

        args = self._base(
            chip=package.chip,
            port=port,
            baud=baud,
            before=before,
            after=after,
        )

        args.append(command)

        for value in package.write_flash_args:
            args.append(self._normalize_arg(value))

        for item in package.flash_files:
            args.extend([item.offset_text, str(item.path)])

        return self._run(args)
