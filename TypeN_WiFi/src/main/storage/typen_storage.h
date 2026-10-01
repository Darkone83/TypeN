#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define TYPEN_SD_ROOT       "/sdcard"
#define TYPEN_DATA_DIR      "/sdcard/typen"
#define TYPEN_MEDIA_DIR     "/sdcard/typen/media"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t typen_storage_start(void);
bool typen_storage_ready(void);

#ifdef __cplusplus
}
#endif
