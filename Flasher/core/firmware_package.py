from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
from typing import Iterable
import zipfile

from .profiles import DeviceProfile


class FirmwarePackageError(RuntimeError):
    pass


MAX_ARCHIVE_FILES = 4096
MAX_ARCHIVE_UNCOMPRESSED_BYTES = 512 * 1024 * 1024


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
    source_path: Path
    _temp_dir: tempfile.TemporaryDirectory[str] | None = field(
        default=None,
        repr=False,
        compare=False,
    )

    @classmethod
    def load(cls, source: str | Path, profile: DeviceProfile) -> "FirmwarePackage":
        source_path = Path(source).expanduser().resolve()
        temp_dir: tempfile.TemporaryDirectory[str] | None = None

        try:
            if source_path.is_dir():
                root = source_path
            elif source_path.is_file() and source_path.suffix.lower() == ".zip":
                root, temp_dir = cls._extract_archive(source_path)
            elif source_path.is_file():
                raise FirmwarePackageError(
                    "Choose a TypeN firmware .zip file or a package folder."
                )
            else:
                raise FirmwarePackageError(
                    f"Firmware package was not found:\n{source_path}"
                )

            package = cls._load_root(
                root=root,
                source_path=source_path,
                profile=profile,
                temp_dir=temp_dir,
            )
            return package
        except Exception:
            if temp_dir is not None:
                temp_dir.cleanup()
            raise

    @classmethod
    def _extract_archive(
        cls,
        archive_path: Path,
    ) -> tuple[Path, tempfile.TemporaryDirectory[str]]:
        temp_dir = tempfile.TemporaryDirectory(prefix="typen_flasher_")
        destination = Path(temp_dir.name)

        try:
            with zipfile.ZipFile(archive_path, "r") as archive:
                infos = archive.infolist()

                if not infos:
                    raise FirmwarePackageError("The firmware ZIP is empty.")

                if len(infos) > MAX_ARCHIVE_FILES:
                    raise FirmwarePackageError(
                        "The firmware ZIP contains an unexpected number of files."
                    )

                total_size = sum(info.file_size for info in infos)
                if total_size > MAX_ARCHIVE_UNCOMPRESSED_BYTES:
                    raise FirmwarePackageError(
                        "The firmware ZIP is unexpectedly large after extraction."
                    )

                for info in infos:
                    if info.flag_bits & 0x1:
                        raise FirmwarePackageError(
                            "Password-protected firmware ZIP files are not supported."
                        )

                    normalized = info.filename.replace("\\", "/")
                    member = PurePosixPath(normalized)

                    if (
                        member.is_absolute()
                        or not member.parts
                        or any(part in ("", ".", "..") for part in member.parts)
                    ):
                        raise FirmwarePackageError(
                            f"Unsafe path in firmware ZIP: {info.filename}"
                        )

                    unix_mode = (info.external_attr >> 16) & 0xFFFF
                    if unix_mode and stat.S_ISLNK(unix_mode):
                        raise FirmwarePackageError(
                            f"Symbolic links are not allowed in firmware ZIPs: {info.filename}"
                        )

                    target = destination.joinpath(*member.parts)

                    if info.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                        continue

                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(info, "r") as src, target.open("wb") as dst:
                        shutil.copyfileobj(src, dst, length=1024 * 1024)

        except zipfile.BadZipFile as exc:
            temp_dir.cleanup()
            raise FirmwarePackageError(
                "The selected file is not a valid ZIP archive."
            ) from exc
        except Exception:
            temp_dir.cleanup()
            raise

        direct = destination / "flasher_args.json"
        if direct.is_file():
            return destination, temp_dir

        manifests = [
            path
            for path in destination.rglob("flasher_args.json")
            if "__MACOSX" not in path.parts
        ]

        if not manifests:
            temp_dir.cleanup()
            raise FirmwarePackageError(
                "flasher_args.json was not found in the firmware ZIP."
            )

        if len(manifests) > 1:
            temp_dir.cleanup()
            raise FirmwarePackageError(
                "The firmware ZIP contains more than one firmware package. "
                "Please choose a ZIP containing a single TypeN package."
            )

        return manifests[0].parent, temp_dir

    @classmethod
    def _load_root(
        cls,
        *,
        root: Path,
        source_path: Path,
        profile: DeviceProfile,
        temp_dir: tempfile.TemporaryDirectory[str] | None,
    ) -> "FirmwarePackage":
        root = root.resolve()
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
                f"This package targets {chip}, but {profile.title} expects "
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
            relative = PurePosixPath(rel_path)

            if (
                relative.is_absolute()
                or not relative.parts
                or any(part in ("", ".", "..") for part in relative.parts)
            ):
                raise FirmwarePackageError(
                    f"Invalid firmware path in flasher_args.json: {rel_path}"
                )

            file_path = root.joinpath(*relative.parts)

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
                rel_path = str(rel).replace("\\", "/")
                relative = PurePosixPath(rel_path)

                if (
                    relative.is_absolute()
                    or any(part in ("", ".", "..") for part in relative.parts)
                ):
                    raise FirmwarePackageError(
                        f"Invalid application path in flasher_args.json: {rel_path}"
                    )

                app_file = root.joinpath(*relative.parts)
                try:
                    app_offset = int(str(off), 0)
                except ValueError:
                    app_offset = None

                if not app_file.is_file():
                    raise FirmwarePackageError(
                        "Application file referenced by flasher_args.json is "
                        f"missing:\n{rel_path}"
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
            source_path=source_path,
            _temp_dir=temp_dir,
        )

    @property
    def is_archive(self) -> bool:
        return self.source_path.is_file() and self.source_path.suffix.lower() == ".zip"

    @property
    def display_name(self) -> str:
        return self.source_path.name

    def close(self) -> None:
        if self._temp_dir is not None:
            self._temp_dir.cleanup()
            self._temp_dir = None

    def summary_lines(self) -> Iterable[str]:
        yield f"Target chip: {self.chip}"
        if self.app_file:
            yield f"Application: {self.app_file.name}"
        yield f"Flash images: {len(self.flash_files)}"
        yield ""
        for item in self.flash_files:
            yield f"{item.offset_text:>10}  {item.relative_path}"
