cmake_minimum_required(VERSION 3.16)

# Embedded TypeN firmware identity. The updater reads this directly from the
# application .bin via esp_app_desc_t.version; no filename or sidecar metadata
# is trusted for firmware identity.
#
# Format: <semver>+<hardware-variant>.abi<firmware-abi>
#   External C6 / UART: 1.0.0+uart.abi1
#   Onboard C6 / SDIO:  1.0.0+sdio.abi1
set(PROJECT_VER "1.0.0+sdio.abi1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(TypeN)