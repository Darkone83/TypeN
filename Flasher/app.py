from __future__ import annotations

from pathlib import Path
import sys

from PySide6.QtCore import QThread
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QComboBox,
    QPlainTextEdit,
    QTabWidget,
    QVBoxLayout,
    QWidget,
    QGroupBox,
)

from serial.tools import list_ports

from core.esptool_runner import EsptoolRunner
from core.firmware_package import FirmwarePackage, FirmwarePackageError
from core.profiles import DeviceProfile, PROFILES
from core.workers import CommandWorker


DEFAULT_BAUD = 460800


class ProfilePage(QWidget):
    def __init__(self, profile: DeviceProfile, parent=None):
        super().__init__(parent)
        self.profile = profile
        self.package: FirmwarePackage | None = None
        self.thread: QThread | None = None
        self.worker: CommandWorker | None = None

        layout = QVBoxLayout(self)

        title = QLabel(f"<h2>{profile.title}</h2><p>{profile.description}</p>")
        title.setWordWrap(True)
        layout.addWidget(title)

        device_group = QGroupBox("Device")
        device_layout = QHBoxLayout(device_group)

        self.port = QComboBox()
        self.baud = QComboBox()
        self.baud.setEditable(True)
        self.baud.addItems(["115200", "460800", "921600"])
        self.baud.setCurrentText(str(DEFAULT_BAUD))

        refresh = QPushButton("Refresh Ports")
        refresh.clicked.connect(self.refresh_ports)

        identify = QPushButton("Identify")
        identify.clicked.connect(self.identify)

        device_layout.addWidget(QLabel("Port"))
        device_layout.addWidget(self.port, 1)
        device_layout.addWidget(QLabel("Baud"))
        device_layout.addWidget(self.baud)
        device_layout.addWidget(refresh)
        device_layout.addWidget(identify)

        layout.addWidget(device_group)

        package_group = QGroupBox("Firmware Package")
        package_layout = QVBoxLayout(package_group)

        row = QHBoxLayout()
        self.package_label = QLabel("No package selected")
        choose = QPushButton("Select Package Folder")
        choose.clicked.connect(self.choose_package)

        row.addWidget(self.package_label, 1)
        row.addWidget(choose)
        package_layout.addLayout(row)

        self.package_summary = QPlainTextEdit()
        self.package_summary.setReadOnly(True)
        self.package_summary.setMaximumHeight(145)
        package_layout.addWidget(self.package_summary)

        layout.addWidget(package_group)

        action_group = QGroupBox("Actions")
        action_layout = QHBoxLayout(action_group)

        self.full_flash_btn = QPushButton("Full Flash / New Board")
        self.full_flash_btn.clicked.connect(self.full_flash)

        self.update_btn = QPushButton("Firmware Update")
        self.update_btn.setEnabled(False)
        self.update_btn.setToolTip(
            "Disabled in the first implementation until the UART updater "
            "reads the device OTA state and can safely choose the bootable "
            "application slot."
        )

        self.dump_btn = QPushButton("Dump Flash")
        self.dump_btn.clicked.connect(self.dump_flash)

        self.erase_btn = QPushButton("Full Erase")
        self.erase_btn.clicked.connect(self.erase_flash)

        action_layout.addWidget(self.full_flash_btn)
        action_layout.addWidget(self.update_btn)
        action_layout.addWidget(self.dump_btn)
        action_layout.addWidget(self.erase_btn)

        layout.addWidget(action_group)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        layout.addWidget(self.log, 1)

        self.refresh_ports()

    def refresh_ports(self) -> None:
        current = self.port.currentText()
        ports = [item.device for item in list_ports.comports()]
        self.port.clear()
        self.port.addItems(ports)
        if current in ports:
            self.port.setCurrentText(current)

    def selected_port(self) -> str:
        port = self.port.currentText().strip()
        if not port:
            raise RuntimeError("Select a serial port first.")
        return port

    def selected_baud(self) -> int:
        try:
            baud = int(self.baud.currentText().strip())
        except ValueError as exc:
            raise RuntimeError("Baud rate must be a number.") from exc
        if baud <= 0:
            raise RuntimeError("Baud rate must be positive.")
        return baud

    def choose_package(self) -> None:
        folder = QFileDialog.getExistingDirectory(
            self,
            f"Select {self.profile.title} firmware package",
        )
        if not folder:
            return

        try:
            package = FirmwarePackage.load(folder, self.profile)
        except FirmwarePackageError as exc:
            QMessageBox.critical(self, "Invalid Firmware Package", str(exc))
            return

        self.package = package
        self.package_label.setText(str(package.root))
        self.package_summary.setPlainText("\n".join(package.summary_lines()))

    def append_log(self, line: str) -> None:
        self.log.appendPlainText(line)
        bar = self.log.verticalScrollBar()
        bar.setValue(bar.maximum())

    def run_operation(self, operation) -> None:
        if self.thread is not None:
            QMessageBox.warning(
                self,
                "Operation Running",
                "Wait for the current operation to finish.",
            )
            return

        thread = QThread(self)
        worker = CommandWorker(operation)
        worker.moveToThread(thread)

        thread.started.connect(worker.run)
        worker.line.connect(self.append_log)
        worker.failed.connect(lambda msg: self.append_log(f"ERROR: {msg}"))
        worker.failed.connect(lambda _msg: thread.quit())
        worker.finished.connect(
            lambda code: self.append_log(
                "Operation completed successfully."
                if code == 0
                else f"Operation exited with code {code}."
            )
        )
        worker.finished.connect(lambda _code: thread.quit())
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.finished.connect(self._operation_done)

        self.thread = thread
        self.worker = worker
        thread.start()

    def _operation_done(self) -> None:
        self.thread = None
        self.worker = None

    def identify(self) -> None:
        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        self.run_operation(
            lambda emit: EsptoolRunner(emit).identify(
                chip=self.profile.chip,
                port=port,
                baud=baud,
            )
        )

    def full_flash(self) -> None:
        if self.package is None:
            QMessageBox.warning(
                self,
                "Firmware Package",
                "Select a valid full-flash package first.",
            )
            return

        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        answer = QMessageBox.question(
            self,
            "Full Flash",
            "This will write the complete firmware layout from "
            "flasher_args.json.\n\nContinue?",
        )
        if answer != QMessageBox.Yes:
            return

        package = self.package
        self.run_operation(
            lambda emit: EsptoolRunner(emit).full_flash(
                package=package,
                port=port,
                baud=baud,
            )
        )

    def erase_flash(self) -> None:
        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        answer = QMessageBox.warning(
            self,
            "Erase Entire Flash",
            "This permanently erases firmware, settings, OTA state and all "
            "other flash contents.\n\nContinue?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if answer != QMessageBox.Yes:
            return

        self.run_operation(
            lambda emit: EsptoolRunner(emit).erase(
                chip=self.profile.chip,
                port=port,
                baud=baud,
            )
        )

    def dump_flash(self) -> None:
        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        suggested = f"{self.profile.key}_flash_dump.bin"
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "Save Full Flash Dump",
            suggested,
            "Binary files (*.bin);;All files (*)",
        )
        if not filename:
            return

        self.run_operation(
            lambda emit: EsptoolRunner(emit).dump(
                chip=self.profile.chip,
                port=port,
                baud=baud,
                output=Path(filename),
            )
        )


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("TypeN Flasher")
        self.resize(1050, 700)

        tabs = QTabWidget()
        for profile in PROFILES:
            tabs.addTab(ProfilePage(profile), profile.title)

        self.setCentralWidget(tabs)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("TypeN Flasher")

    window = MainWindow()
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
