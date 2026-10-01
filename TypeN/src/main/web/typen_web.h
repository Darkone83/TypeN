#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*typen_web_select_cb_t)(const char *name, void *ctx);

esp_err_t typen_web_start(typen_web_select_cb_t select_cb, void *ctx);

#ifdef __cplusplus
}
#endif
