from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QObject, Signal, Slot


class CommandWorker(QObject):
    line = Signal(str)
    finished = Signal(int)
    failed = Signal(str)

    def __init__(self, operation: Callable[[Callable[[str], None]], int]):
        super().__init__()
        self.operation = operation

    @Slot()
    def run(self) -> None:
        try:
            code = int(self.operation(self.line.emit))
            self.finished.emit(code)
        except Exception as exc:
            self.failed.emit(str(exc))
