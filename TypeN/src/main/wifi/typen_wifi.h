#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t typen_wifi_start(void);
const char *typen_wifi_ip_string(void);

/*
 * Enables the captive-portal layer after the HTTP manager is ready:
 * - advertises this device as DNS through DHCP
 * - advertises the portal URL through DHCP option 114
 * - answers A-record DNS queries with the TypeN SoftAP address
 */
esp_err_t typen_wifi_enable_captive_portal(void);

#ifdef __cplusplus
}
#endif
