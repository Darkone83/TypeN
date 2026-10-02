#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hidden TypeN wireframe Easter egg.
 *
 * The renderer runs on its own task and uses the normal UI framebuffer.
 * Playback must be paused by the caller before starting it.
 */
esp_err_t typen_ui_misc_start(void);

void typen_ui_misc_request_exit(void);

bool typen_ui_misc_is_active(void);

/*
 * EXIT button hit-test used by the normal touch task while the animation is
 * active. The misc renderer owns drawing; the UI task remains the only touch
 * reader.
 */
bool typen_ui_misc_exit_hit(uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
