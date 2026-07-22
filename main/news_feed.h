#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NEWS_FEED_MAX_URL      512
#define NEWS_FEED_MAX_ITEMS    6
#define NEWS_FEED_TITLE_LEN    128
#define NEWS_FEED_SUMMARY_LEN  320
#define NEWS_FEED_SOURCE_LEN   48
#define NEWS_FEED_TIME_LEN     32

typedef struct {
    bool     enabled;
    char     source_url[NEWS_FEED_MAX_URL];
    uint32_t refresh_sec;
} news_feed_config_t;

typedef struct {
    char title[NEWS_FEED_TITLE_LEN];
    char summary[NEWS_FEED_SUMMARY_LEN];
    char source[NEWS_FEED_SOURCE_LEN];
    char time[NEWS_FEED_TIME_LEN];
} news_feed_item_t;

typedef struct {
    bool             valid;
    char             page_title[NEWS_FEED_SOURCE_LEN];
    char             updated_at[NEWS_FEED_TIME_LEN];
    news_feed_item_t items[NEWS_FEED_MAX_ITEMS];
    int              item_count;
    int              current_index;
} news_feed_data_t;

esp_err_t news_feed_init(void);
esp_err_t news_feed_get_config(news_feed_config_t *out);
esp_err_t news_feed_set_config(const news_feed_config_t *cfg);
bool      news_feed_config_ready(void);

esp_err_t news_feed_fetch_now(void);
esp_err_t news_feed_show(void);
esp_err_t news_feed_show_next(void);
esp_err_t news_feed_refresh_and_show(void);
void      news_feed_get_data_copy(news_feed_data_t *out);
