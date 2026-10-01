from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Iterable

from .profiles import DeviceProfile


class FirmwarePackageError(RuntimeError):
    pass


@dataclass(frozen=True)
class FlashFile:
    offset_text: str
    offset: int
    relative_path: str
    path: Path


@dataclass
class FirmwarePackage:
    root: Path
    args: dict
    chip: str
    write_flash_args: list[str]
    flash_files: list[FlashFile]
    app_file: Path | None
    app_offset: int | None

    @classmethod
    def load(cls, root: str | Path, profile: DeviceProfile) -> "FirmwarePackage":
        root = Path(root).expanduser().resolve()
        args_path = root / "flasher_args.json"

        if not args_path.is_file():
            raise FirmwarePackageError(
                f"flasher_args.json was not found in:\n{root}"
            )

        try:
            args = json.loads(args_path.read_text(encoding="utf-8"))
        except Exception as exc:
            raise FirmwarePackageError(
                f"Unable to parse {args_path.name}: {exc}"
            ) from exc

        extra = args.get("extra_esptool_args") or {}
        chip = str(extra.get("chip") or "").strip().lower()

        if not chip:
            raise FirmwarePackageError(
                "flasher_args.json does not identify an ESP target chip."
            )

        if chip != profile.chip:
            raise FirmwarePackageError(
                f"Package target is {chip}, but the selected profile expects "
                f"{profile.chip}."
            )

        raw_files = args.get("flash_files")
        if not isinstance(raw_files, dict) or not raw_files:
            raise FirmwarePackageError(
                "flasher_args.json does not contain flash_files."
            )

        flash_files: list[FlashFile] = []

        for offset_text, rel in raw_files.items():
            try:
                offset = int(str(offset_text), 0)
            except ValueError as exc:
                raise FirmwarePackageError(
                    f"Invalid flash offset: {offset_text}"
                ) from exc

            rel_path = str(rel).replace("\\", "/")
            file_path = root / Path(rel_path)

            if not file_path.is_file():
                raise FirmwarePackageError(
                    "Required full-flash file is missing:\n"
                    f"{rel_path}\n\nExpected at:\n{file_path}"
                )

            flash_files.append(
                FlashFile(
                    offset_text=str(offset_text),
                    offset=offset,
                    relative_path=rel_path,
                    path=file_path,
                )
            )

        flash_files.sort(key=lambda item: item.offset)

        app_file = None
        app_offset = None
        app = args.get("app")

        if isinstance(app, dict):
            rel = app.get("file")
            off = app.get("offset")

            if rel and off:
                app_file = root / Path(str(rel).replace("\\", "/"))
                try:
                    app_offset = int(str(off), 0)
                except ValueError:
                    app_offset = None

                if not app_file.is_file():
                    raise FirmwarePackageError(
                        "Application file referenced by flasher_args.json is "
                        f"missing:\n{rel}"
                    )

        write_flash_args = [
            str(value)
            for value in (args.get("write_flash_args") or [])
        ]

        return cls(
            root=root,
            args=args,
            chip=chip,
            write_flash_args=write_flash_args,
            flash_files=flash_files,
            app_file=app_file,
            app_offset=app_offset,
        )

    def summary_lines(self) -> Iterable[str]:
        yield f"Target: {self.chip}"
        if self.app_file:
            yield f"Application: {self.app_file.name}"
        yield "Full flash:"
        for item in self.flash_files:
            yield f"  {item.offset_text:>10}  {item.relative_path}"
