#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t speaker_test_play_tone(uint8_t volume_percent);

#ifdef __cplusplus
}
#endif
