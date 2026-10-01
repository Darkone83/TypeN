from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class DeviceProfile:
    key: str
    title: str
    chip: str
    expected_project: str | None
    description: str


PROFILES = [
    DeviceProfile(
        key="p4_pico",
        title="P4 Pico",
        chip="esp32p4",
        expected_project=None,
        description=(
            "TypeN P4 Pico profile. Full-flash layout is read from the "
            "selected package's flasher_args.json."
        ),
    ),
    DeviceProfile(
        key="p4_pico_wifi",
        title="P4 Pico WiFi",
        chip="esp32p4",
        expected_project="TypeN",
        description=(
            "TypeN ESP32-P4 host firmware for the WiFi configuration."
        ),
    ),
    DeviceProfile(
        key="c6_slave",
        title="C6 Slave",
        chip="esp32c6",
        expected_project="network_adapter",
        description=(
            "ESP32-C6 ESP-Hosted UART slave firmware."
        ),
    ),
]
