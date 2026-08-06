#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_PLAYER_STOPPED = 0,
    MUSIC_PLAYER_PLAYING,
    MUSIC_PLAYER_PAUSED,
    MUSIC_PLAYER_STOPPING,
    MUSIC_PLAYER_ERROR,
} music_player_state_t;

typedef struct {
    music_player_state_t state;
    uint8_t volume_percent;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t elapsed_ms;
    uint64_t data_bytes_total;
    uint64_t data_bytes_played;
    char track[96];
    char last_error[32];
} music_player_status_t;

esp_err_t music_player_init(void);
esp_err_t music_player_play(const char *filename);
esp_err_t music_player_pause(void);
esp_err_t music_player_resume(void);
esp_err_t music_player_stop(void);
esp_err_t music_player_set_volume(uint8_t percent);
void music_player_get_status(music_player_status_t *out);
const char *music_player_state_name(music_player_state_t state);
bool music_player_is_supported_file(const char *filename);
bool music_player_valid_filename(const char *filename);
esp_err_t music_player_validate_wav_file(const char *path);
esp_err_t music_player_validate_file(const char *path, const char *filename);

#ifdef __cplusplus
}
#endif
