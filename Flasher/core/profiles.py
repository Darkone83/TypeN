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
        title="TypeN P4 Pico",
        chip="esp32p4",
        expected_project=None,
        description=(
            "TypeN display using a P4 Pico with an external ESP32-C6 "
            "Wi-Fi module."
        ),
    ),
    DeviceProfile(
        key="p4_pico_wifi",
        title="TypeN P4 Pico WiFi",
        chip="esp32p4",
        expected_project="TypeN",
        description=(
            "TypeN display using the P4 Pico WiFi with its onboard "
            "ESP32-C6."
        ),
    ),
    DeviceProfile(
        key="c6_slave",
        title="C6 Wi-Fi Module",
        chip="esp32c6",
        expected_project="network_adapter",
        description=(
            "Firmware for the external ESP32-C6 Wi-Fi module used with "
            "the TypeN P4 Pico."
        ),
    ),
]
