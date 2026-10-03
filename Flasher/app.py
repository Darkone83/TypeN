from __future__ import annotations

from pathlib import Path
import sys

from PySide6.QtCore import QThread, Qt
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from serial.tools import list_ports

from core.esptool_runner import EsptoolRunner
from core.firmware_package import FirmwarePackage, FirmwarePackageError
from core.profiles import DeviceProfile, PROFILES
from core.workers import CommandWorker


DEFAULT_BAUD = 460800


APP_STYLE = """
QWidget {
    background: #0b0d10;
    color: #e9edf2;
    font-family: "Segoe UI", "Inter", sans-serif;
    font-size: 13px;
}
QMainWindow, QTabWidget::pane {
    background: #0b0d10;
}
QTabWidget::pane {
    border: 0;
    margin-top: 6px;
}
QTabBar::tab {
    background: #14181d;
    color: #9ca7b3;
    padding: 11px 18px;
    margin-right: 4px;
    border-radius: 7px;
}
QTabBar::tab:selected {
    background: #242b33;
    color: #ffffff;
}
QFrame#Card {
    background: #12161b;
    border: 1px solid #252b33;
    border-radius: 12px;
}
QLabel {
    background: transparent;
}
QLabel#HeroTitle {
    font-size: 26px;
    font-weight: 700;
    color: #ffffff;
}
QLabel#SectionTitle {
    font-size: 16px;
    font-weight: 650;
    color: #ffffff;
    padding-bottom: 2px;
}
QLabel#Muted {
    color: #909aa5;
}
QLabel#Ready {
    color: #91e6b2;
    font-weight: 600;
}
QLabel#Warning {
    color: #f5c56b;
    font-weight: 600;
}
QPushButton {
    min-height: 36px;
    padding: 0 15px;
    border-radius: 8px;
    border: 1px solid #333b45;
    background: #1a2026;
    color: #e9edf2;
}
QPushButton:hover {
    background: #222a32;
}
QPushButton:disabled {
    background: #111419;
    color: #5e6771;
    border-color: #20252b;
}
QPushButton#Primary {
    min-height: 44px;
    background: #7a43d6;
    border: 1px solid #8d58e1;
    color: white;
    font-size: 14px;
    font-weight: 700;
}
QPushButton#Primary:hover {
    background: #8950e2;
}
QPushButton#Danger {
    color: #ffb7b7;
    border-color: #6b3333;
}
QComboBox, QPlainTextEdit {
    background: #0e1115;
    border: 1px solid #303741;
    border-radius: 7px;
    padding: 6px;
    selection-background-color: #7040c4;
}
QComboBox {
    min-height: 32px;
}
QGroupBox {
    border: 1px solid #252b33;
    border-radius: 10px;
    margin-top: 12px;
    padding-top: 12px;
    color: #b7c0ca;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 5px;
}
"""


def make_card() -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setObjectName("Card")
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(18, 14, 18, 14)
    layout.setSpacing(8)
    return frame, layout


def muted(text: str) -> QLabel:
    label = QLabel(text)
    label.setObjectName("Muted")
    label.setWordWrap(True)
    return label


class ProfilePage(QWidget):
    def __init__(self, profile: DeviceProfile, parent=None):
        super().__init__(parent)
        self.profile = profile
        self.package: FirmwarePackage | None = None
        self.thread: QThread | None = None
        self.worker: CommandWorker | None = None
        self._busy = False

        self.setAcceptDrops(True)

        root = QVBoxLayout(self)
        root.setContentsMargins(24, 16, 24, 18)
        root.setSpacing(12)

        hero = QVBoxLayout()
        hero.setSpacing(4)
        title = QLabel(profile.title)
        title.setObjectName("HeroTitle")
        subtitle = muted(profile.description)
        hero.addWidget(title)
        hero.addWidget(subtitle)
        root.addLayout(hero)

        columns = QHBoxLayout()
        columns.setSpacing(14)

        left_column = QVBoxLayout()
        left_column.setSpacing(12)

        right_column = QVBoxLayout()
        right_column.setSpacing(12)

        # 1. Connect
        device_card, device_layout = make_card()
        device_title = QLabel("1. Connect your device")
        device_title.setObjectName("SectionTitle")
        device_layout.addWidget(device_title)
        device_layout.addWidget(
            muted("Connect the device by USB, then choose its serial port.")
        )

        device_row = QHBoxLayout()
        device_row.setSpacing(10)
        self.port = QComboBox()
        self.port.setMinimumWidth(320)

        self.refresh_btn = QPushButton("Refresh")
        self.refresh_btn.clicked.connect(self.refresh_ports)

        self.identify_btn = QPushButton("Check Connection")
        self.identify_btn.clicked.connect(self.identify)

        device_row.addWidget(self.port, 1)
        device_row.addWidget(self.refresh_btn)
        device_row.addWidget(self.identify_btn)
        device_layout.addLayout(device_row)
        device_layout.addSpacing(2)

        self.connection_status = QLabel("Device connection not checked")
        self.connection_status.setObjectName("Muted")
        device_layout.addWidget(self.connection_status)
        left_column.addWidget(device_card)

        # 2. Firmware package
        package_card, package_layout = make_card()
        package_title = QLabel("2. Choose firmware")
        package_title.setObjectName("SectionTitle")
        package_layout.addWidget(package_title)
        package_layout.addWidget(
            muted(
                "Choose a TypeN firmware ZIP. Package folders are also "
                "supported for development builds. You can also drag a ZIP "
                "onto this window."
            )
        )

        self.package_label = QLabel("No firmware package selected")
        self.package_label.setObjectName("Muted")
        self.package_label.setWordWrap(True)
        package_layout.addWidget(self.package_label)

        package_actions = QHBoxLayout()
        package_actions.setSpacing(10)

        self.choose_zip_btn = QPushButton("Choose Firmware Package…")
        self.choose_zip_btn.setObjectName("Primary")
        self.choose_zip_btn.clicked.connect(self.choose_package_zip)

        self.choose_folder_btn = QPushButton("Use Package Folder")
        self.choose_folder_btn.clicked.connect(self.choose_package_folder)

        package_actions.addWidget(self.choose_zip_btn, 1)
        package_actions.addWidget(self.choose_folder_btn)
        package_layout.addLayout(package_actions)

        package_layout.addSpacing(2)

        self.package_status = QLabel("Waiting for a firmware package")
        self.package_status.setObjectName("Muted")
        self.package_status.setWordWrap(True)
        package_layout.addWidget(self.package_status)

        self.details_btn = QPushButton("Show package details")
        self.details_btn.setCheckable(True)
        self.details_btn.toggled.connect(self._toggle_package_details)
        self.details_btn.setEnabled(False)
        package_layout.addWidget(self.details_btn, 0, Qt.AlignLeft)

        self.package_summary = QPlainTextEdit()
        self.package_summary.setReadOnly(True)
        self.package_summary.setMaximumHeight(120)
        self.package_summary.setVisible(False)
        package_layout.addWidget(self.package_summary)

        left_column.addWidget(package_card)
        left_column.addStretch(1)

        # 3. Install
        install_card, install_layout = make_card()
        install_title = QLabel("3. Install firmware")
        install_title.setObjectName("SectionTitle")
        install_layout.addWidget(install_title)
        install_layout.addWidget(
            muted(
                "The flasher uses the exact chip, flash settings, offsets, "
                "and filenames supplied by the firmware package."
            )
        )
        install_layout.addSpacing(4)

        self.full_flash_btn = QPushButton("Install Firmware")
        self.full_flash_btn.setObjectName("Primary")
        self.full_flash_btn.clicked.connect(self.full_flash)
        self.full_flash_btn.setEnabled(False)
        install_layout.addWidget(self.full_flash_btn)
        right_column.addWidget(install_card)

        # Activity / status
        activity_card, activity_layout = make_card()
        activity_header = QHBoxLayout()
        activity_header.setSpacing(10)
        activity_title = QLabel("Activity")
        activity_title.setObjectName("SectionTitle")
        self.activity_status = QLabel("Ready")
        self.activity_status.setObjectName("Ready")
        activity_header.addWidget(activity_title)
        activity_header.addStretch(1)
        activity_header.addWidget(self.activity_status)
        activity_layout.addLayout(activity_header)

        self.log_toggle = QPushButton("Show technical log")
        self.log_toggle.setCheckable(True)
        self.log_toggle.toggled.connect(self._toggle_log)
        activity_layout.addWidget(self.log_toggle, 0, Qt.AlignLeft)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(140)
        self.log.setVisible(False)
        activity_layout.addWidget(self.log)
        right_column.addWidget(activity_card)

        # Advanced / recovery tools
        self.advanced_group = QGroupBox("Advanced / Recovery Tools")
        self.advanced_group.setCheckable(True)
        self.advanced_group.setChecked(False)
        advanced_outer = QVBoxLayout(self.advanced_group)
        advanced_outer.setContentsMargins(14, 12, 14, 12)
        advanced_outer.setSpacing(8)

        self.advanced_content = QWidget()
        advanced_layout = QGridLayout(self.advanced_content)
        advanced_layout.setContentsMargins(0, 4, 0, 0)
        advanced_layout.setHorizontalSpacing(10)
        advanced_layout.setVerticalSpacing(6)

        self.baud = QComboBox()
        self.baud.setEditable(True)
        self.baud.addItems(["115200", "460800", "921600"])
        self.baud.setCurrentText(str(DEFAULT_BAUD))

        self.dump_btn = QPushButton("Save Full Flash Backup…")
        self.dump_btn.clicked.connect(self.dump_flash)

        self.erase_btn = QPushButton("Erase Device")
        self.erase_btn.setObjectName("Danger")
        self.erase_btn.clicked.connect(self.erase_flash)

        self.update_btn = QPushButton("Direct Firmware Update")
        self.update_btn.setEnabled(False)
        self.update_btn.setToolTip(
            "Reserved for a future safe direct-update workflow that reads "
            "the device OTA state before selecting a destination slot."
        )

        advanced_layout.addWidget(QLabel("Serial speed"), 0, 0)
        advanced_layout.addWidget(self.baud, 0, 1)
        advanced_layout.addWidget(self.dump_btn, 1, 0)
        advanced_layout.addWidget(self.erase_btn, 1, 1)
        advanced_layout.addWidget(self.update_btn, 2, 0, 1, 2)

        advanced_outer.addWidget(self.advanced_content)
        self.advanced_content.setVisible(False)
        self.advanced_group.toggled.connect(self.advanced_content.setVisible)
        right_column.addWidget(self.advanced_group)
        right_column.addStretch(1)

        columns.addLayout(left_column, 1)
        columns.addLayout(right_column, 1)
        root.addLayout(columns, 1)

        self.refresh_ports()
        self._refresh_action_state()

    # ------------------------------------------------------------------
    # Drag/drop firmware ZIPs
    # ------------------------------------------------------------------

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        urls = event.mimeData().urls()
        if len(urls) == 1:
            path = Path(urls[0].toLocalFile())
            if path.is_dir() or path.suffix.lower() == ".zip":
                event.acceptProposedAction()
                return
        event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:
        urls = event.mimeData().urls()
        if len(urls) != 1:
            return
        path = Path(urls[0].toLocalFile())
        self._load_package(path)
        event.acceptProposedAction()

    # ------------------------------------------------------------------
    # UI helpers
    # ------------------------------------------------------------------

    def _toggle_package_details(self, checked: bool) -> None:
        self.package_summary.setVisible(checked)
        self.details_btn.setText(
            "Hide package details" if checked else "Show package details"
        )

    def _toggle_log(self, checked: bool) -> None:
        self.log.setVisible(checked)
        self.log_toggle.setText(
            "Hide technical log" if checked else "Show technical log"
        )

    def _refresh_action_state(self) -> None:
        has_port = self.port.count() > 0 and bool(self.port.currentData())
        has_package = self.package is not None

        self.full_flash_btn.setEnabled(
            has_port and has_package and not self._busy
        )
        self.identify_btn.setEnabled(has_port and not self._busy)
        self.refresh_btn.setEnabled(not self._busy)
        self.choose_zip_btn.setEnabled(not self._busy)
        self.choose_folder_btn.setEnabled(not self._busy)
        self.dump_btn.setEnabled(has_port and not self._busy)
        self.erase_btn.setEnabled(has_port and not self._busy)
        self.port.setEnabled(not self._busy)
        self.baud.setEnabled(not self._busy)

    def _set_busy(self, busy: bool, message: str | None = None) -> None:
        self._busy = busy
        if message:
            self.activity_status.setText(message)
            self.activity_status.setObjectName("Warning" if busy else "Ready")
            self.activity_status.style().unpolish(self.activity_status)
            self.activity_status.style().polish(self.activity_status)
        self._refresh_action_state()

    def refresh_ports(self) -> None:
        current = self.port.currentData()
        devices = list(list_ports.comports())

        self.port.blockSignals(True)
        self.port.clear()

        for item in devices:
            description = (item.description or "Serial device").strip()
            label = f"{item.device} — {description}"
            self.port.addItem(label, item.device)

        if current:
            for index in range(self.port.count()):
                if self.port.itemData(index) == current:
                    self.port.setCurrentIndex(index)
                    break

        self.port.blockSignals(False)

        if not devices:
            self.connection_status.setText(
                "No serial device detected. Connect TypeN by USB and press Refresh."
            )
            self.connection_status.setObjectName("Warning")
        else:
            self.connection_status.setText(
                f"{len(devices)} serial device{'s' if len(devices) != 1 else ''} detected"
            )
            self.connection_status.setObjectName("Muted")

        self.connection_status.style().unpolish(self.connection_status)
        self.connection_status.style().polish(self.connection_status)
        self._refresh_action_state()

    def selected_port(self) -> str:
        port = self.port.currentData()
        if not port:
            raise RuntimeError("Connect the device and select its USB serial port first.")
        return str(port)

    def selected_baud(self) -> int:
        try:
            baud = int(self.baud.currentText().strip())
        except ValueError as exc:
            raise RuntimeError("Serial speed must be a number.") from exc
        if baud <= 0:
            raise RuntimeError("Serial speed must be positive.")
        return baud

    # ------------------------------------------------------------------
    # Package selection
    # ------------------------------------------------------------------

    def choose_package_zip(self) -> None:
        filename, _ = QFileDialog.getOpenFileName(
            self,
            f"Choose {self.profile.title} firmware",
            "",
            "TypeN firmware package (*.zip);;All files (*)",
        )
        if filename:
            self._load_package(Path(filename))

    def choose_package_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(
            self,
            f"Choose {self.profile.title} firmware folder",
        )
        if folder:
            self._load_package(Path(folder))

    def _load_package(self, source: Path) -> None:
        try:
            package = FirmwarePackage.load(source, self.profile)
        except FirmwarePackageError as exc:
            QMessageBox.critical(self, "Firmware Package", str(exc))
            return

        old_package = self.package
        self.package = package
        if old_package is not None:
            old_package.close()

        self.package_label.setText(str(package.source_path))
        app_name = package.app_file.name if package.app_file else "application image"
        source_kind = "ZIP package" if package.is_archive else "package folder"
        self.package_status.setText(
            f"Ready — {source_kind}, {app_name}, "
            f"{len(package.flash_files)} flash image"
            f"{'s' if len(package.flash_files) != 1 else ''}"
        )
        self.package_status.setObjectName("Ready")
        self.package_status.style().unpolish(self.package_status)
        self.package_status.style().polish(self.package_status)

        self.package_summary.setPlainText("\n".join(package.summary_lines()))
        self.details_btn.setEnabled(True)
        self._refresh_action_state()

    # ------------------------------------------------------------------
    # Operation runner
    # ------------------------------------------------------------------

    def append_log(self, line: str) -> None:
        self.log.appendPlainText(line)
        bar = self.log.verticalScrollBar()
        bar.setValue(bar.maximum())

    def run_operation(self, operation, label: str) -> None:
        if self.thread is not None:
            QMessageBox.warning(
                self,
                "Operation Running",
                "Wait for the current operation to finish.",
            )
            return

        self._set_busy(True, label)

        thread = QThread(self)
        worker = CommandWorker(operation)
        worker.moveToThread(thread)

        thread.started.connect(worker.run)
        worker.line.connect(self.append_log)
        worker.failed.connect(self._operation_failed)
        worker.failed.connect(lambda _msg: thread.quit())
        worker.finished.connect(self._operation_finished)
        worker.finished.connect(lambda _code: thread.quit())
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.finished.connect(self._operation_done)

        self.thread = thread
        self.worker = worker
        thread.start()

    def _operation_failed(self, message: str) -> None:
        self.append_log(f"ERROR: {message}")
        self.activity_status.setText("Operation failed")
        self.activity_status.setObjectName("Warning")
        self.activity_status.style().unpolish(self.activity_status)
        self.activity_status.style().polish(self.activity_status)
        self.log_toggle.setChecked(True)

    def _operation_finished(self, code: int) -> None:
        if code == 0:
            self.append_log("Operation completed successfully.")
            self.activity_status.setText("Completed successfully")
            self.activity_status.setObjectName("Ready")
        else:
            self.append_log(f"Operation exited with code {code}.")
            self.activity_status.setText(f"Operation failed (code {code})")
            self.activity_status.setObjectName("Warning")
            self.log_toggle.setChecked(True)

        self.activity_status.style().unpolish(self.activity_status)
        self.activity_status.style().polish(self.activity_status)

    def _operation_done(self) -> None:
        self.thread = None
        self.worker = None
        self._busy = False
        self._refresh_action_state()

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def identify(self) -> None:
        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        self.connection_status.setText("Checking device connection…")
        self.connection_status.setObjectName("Warning")
        self.connection_status.style().unpolish(self.connection_status)
        self.connection_status.style().polish(self.connection_status)

        self.run_operation(
            lambda emit: EsptoolRunner(emit).identify(
                chip=self.profile.chip,
                port=port,
                baud=baud,
            ),
            "Checking connection…",
        )

    def full_flash(self) -> None:
        if self.package is None:
            QMessageBox.warning(
                self,
                "Firmware Package",
                "Choose a valid firmware package first.",
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
            "Install Firmware",
            f"Install {self.package.display_name} on the connected "
            f"{self.profile.title}?\n\n"
            "Keep the device connected until installation finishes.",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if answer != QMessageBox.Yes:
            return

        package = self.package
        self.run_operation(
            lambda emit: EsptoolRunner(emit).full_flash(
                package=package,
                port=port,
                baud=baud,
            ),
            "Installing firmware…",
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
            "Erase Device",
            "This completely erases the connected device, including firmware, "
            "settings, and OTA state. You will need to install firmware again.\n\n"
            "Continue?",
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
            ),
            "Erasing device…",
        )

    def dump_flash(self) -> None:
        try:
            port = self.selected_port()
            baud = self.selected_baud()
        except RuntimeError as exc:
            QMessageBox.warning(self, "Device", str(exc))
            return

        suggested = f"{self.profile.key}_flash_backup.bin"
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "Save Full Flash Backup",
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
            ),
            "Saving flash backup…",
        )

    def cleanup(self) -> None:
        if self.package is not None:
            self.package.close()
            self.package = None


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("TypeN Firmware Installer")
        self.resize(1240, 780)
        self.setMinimumSize(1040, 700)

        shell = QWidget()
        shell_layout = QVBoxLayout(shell)
        shell_layout.setContentsMargins(20, 14, 20, 16)
        shell_layout.setSpacing(6)

        brand = QHBoxLayout()
        brand_title = QLabel("TypeN Firmware Installer")
        brand_title.setObjectName("HeroTitle")
        brand_note = muted("Install or recover TypeN firmware over USB")
        brand_note.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        brand.addWidget(brand_title)
        brand.addStretch(1)
        brand.addWidget(brand_note)
        shell_layout.addLayout(brand)

        self.tabs = QTabWidget()
        self.pages: list[ProfilePage] = []
        for profile in PROFILES:
            page = ProfilePage(profile)
            self.pages.append(page)
            tab_name = (
                "P4 Pico"
                if profile.key == "p4_pico"
                else "P4 Pico WiFi"
                if profile.key == "p4_pico_wifi"
                else "C6 Wi-Fi Module"
            )
            self.tabs.addTab(page, tab_name)

        shell_layout.addWidget(self.tabs, 1)
        self.setCentralWidget(shell)

    def closeEvent(self, event) -> None:
        if any(page.thread is not None for page in self.pages):
            QMessageBox.warning(
                self,
                "Operation Running",
                "Wait for the current flashing operation to finish before closing.",
            )
            event.ignore()
            return

        for page in self.pages:
            page.cleanup()

        event.accept()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("TypeN Firmware Installer")
    app.setStyleSheet(APP_STYLE)

    window = MainWindow()
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
