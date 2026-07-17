#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT_POINT "/sdcard"
#define SD_CARD_APP_DIR     SD_CARD_MOUNT_POINT "/epd"
#define SD_CARD_IMAGES_DIR  SD_CARD_APP_DIR "/images"
#define SD_CARD_BACKUP_DIR  SD_CARD_APP_DIR "/backup"
#define SD_CARD_LOGS_DIR    SD_CARD_APP_DIR "/logs"

#define SD_CARD_PWR_EN_GPIO GPIO_NUM_21
#define SD_CARD_CLK_GPIO    GPIO_NUM_48
#define SD_CARD_CMD_GPIO    GPIO_NUM_47
#define SD_CARD_D0_GPIO     GPIO_NUM_41

typedef struct {
    bool initialized;
    bool powered;
    bool mounted;
    bool card_present;
    bool dirs_ready;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t sector_size;
    uint32_t capacity_mb;
    char card_name[16];
    esp_err_t last_error;
    esp_err_t last_dir_error;
} sd_card_status_t;

esp_err_t sd_card_init(void);
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
esp_err_t sd_card_prepare_sleep(void);
void sd_card_get_status(sd_card_status_t *out);
const char *sd_card_mount_point(void);

#ifdef __cplusplus
}
#endif
