# TypeN — P4 Pico + External C6 Slave Wiring

This wiring guide is for the **TypeN P4 Pico + external ESP32-C6 slave** combination.

It does **not** apply to the **P4 Pico WiFi / onboard-C6 variant**, which uses the board's native SDIO connection between the ESP32-P4 and ESP32-C6.

## Connection type

The external C6 slave configuration uses **ESP-Hosted over UART**.

The ESP32-P4 remains the TypeN host application processor and the ESP32-C6 acts as the Wi-Fi coprocessor.

## UART wiring

| TypeN P4 Pico | Direction | External ESP32-C6 Slave |
|---|---|---|
| GPIO18 — TX | P4 → C6 | GPIO20 — RX |
| GPIO19 — RX | C6 → P4 | GPIO21 — TX |
| GPIO54 — C6 Reset | P4 → C6 | EN / CHIP_PU |
| GND | Common | GND |

In schematic form:

```text
ESP32-P4 Pico                         ESP32-C6 Slave
────────────────────────────────────────────────────────

GPIO18  TX  ───────────────────────► GPIO20  RX

GPIO19  RX  ◄─────────────────────── GPIO21  TX

GPIO54      ───────────────────────► EN / CHIP_PU

GND         ──────────────────────── GND
```

TX and RX are crossed as expected:

```text
P4 TX -> C6 RX
P4 RX <- C6 TX
```

## Reset line

`GPIO54` on the ESP32-P4 is used to control the C6 slave reset/enable input.

Connect:

```text
P4 GPIO54 -> C6 EN / CHIP_PU
```

The TypeN host firmware uses this line when bringing up or resetting the ESP-Hosted slave.

## Power

The P4 Pico and C6 board must share a common ground.

They may be powered from the same **5 V / USB supply**, provided the power arrangement is appropriate for both boards.

### Important

Do **not** connect the two boards' regulated `3V3` outputs together.

Recommended arrangement:

```text
Shared 5 V / USB source
        ├── P4 Pico power input
        └── C6 board power input

Shared GND
```

Each board should continue using its own onboard 3.3 V regulator.

## C6 USB connection

The C6 board's USB connection may remain available for:

- initial C6 firmware flashing
- C6 serial/debug output
- recovery/reflashing

The normal TypeN host-to-C6 data path is still the UART connection shown above.

## TypeN host configuration

For the **P4 Pico + external C6 slave** firmware, ESP-Hosted should remain configured for UART transport.

The working TypeN configuration uses:

```text
P4 UART TX     GPIO18
P4 UART RX     GPIO19
C6 reset       GPIO54
```

The exact UART baud rate and queue/checksum settings are firmware configuration details and should be kept matched between the P4 host and C6 slave builds.

## C6 slave configuration

The external C6 slave firmware must be built for **ESP-Hosted UART slave mode**.

For the current external C6 wiring:

```text
C6 RX = GPIO20
C6 TX = GPIO21
```

These are the C6-side pins connected to the P4 host.

## microSD relationship

This external-C6 UART arrangement does **not** use the ESP32-P4 SDMMC peripheral for the C6 link.

That means the C6 transport is electrically independent from the TypeN microSD interface.

```text
C6 link  -> UART
microSD  -> SDMMC
```

This is one of the key differences from the P4 Pico WiFi / onboard-C6 variant, where the C6 uses SDIO.

## Variant warning

Do not use this wiring for the onboard-C6 P4 WiFi model.

### P4 Pico + external C6 slave

```text
Transport: UART

P4 GPIO18 -> C6 GPIO20
P4 GPIO19 <- C6 GPIO21
P4 GPIO54 -> C6 EN
```

### P4 Pico WiFi / onboard C6

```text
Transport: SDIO

CLK   GPIO18
CMD   GPIO19
D0    GPIO14
D1    GPIO15
D2    GPIO16
D3    GPIO17
RESET GPIO54
```

The two TypeN firmware variants therefore require different ESP-Hosted transport configurations.

## Bring-up checklist

Before powering the assembled combination:

- Verify `P4 GPIO18` connects to `C6 GPIO20`.
- Verify `P4 GPIO19` connects to `C6 GPIO21`.
- Verify `P4 GPIO54` connects to `C6 EN / CHIP_PU`.
- Verify both boards share ground.
- Verify TX/RX are crossed, not TX-to-TX.
- Verify the two `3V3` regulator outputs are **not** tied together.
- Confirm the P4 firmware is the **UART C6** TypeN build.
- Confirm the C6 firmware is the **UART slave** build.
- Keep C6 USB accessible during initial bring-up if possible.

## Expected architecture

```text
                TypeN
                  │
          ┌───────┴────────┐
          │                │
      ESP32-P4          ESP32-C6
        Host              Slave
          │                │
          └──── UART ──────┘
                 │
               Wi-Fi

P4 also independently handles:
- display
- touch
- microSD
- media playback
- local UI
- OTA/update logic
```

This document is specifically for the **discrete P4 Pico + C6 slave** hardware configuration.
