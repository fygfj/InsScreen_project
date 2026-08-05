#pragma once

#include "esp_err.h"

/**
 * Rotary encoder, active-low with external pull-ups:
 *   A = GPIO4, B = GPIO18, S = GPIO0
 *
 * Display mode cycle:
 *   Rotate left/right : previous/next mode
 *   Press             : refresh current mode
 */

esp_err_t button_init(void);

int  button_get_current_mode(void);
void button_set_current_mode(int mode);
