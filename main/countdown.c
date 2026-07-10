#include "countdown.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "epd.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "time_sync.h"
#include "display_policy.h"
#include "scheduler.h"

static const char *TAG = "countdown";

#define NVS_NS   "countdown"
#define NVS_BLOB "cfg"

static countdown_config_t s_cfg;
static SemaphoreHandle_t  s_cfg_mutex;
static uint8_t s_page_idx;

/* NVS persistence */

static void nvs_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_cfg);
    if (nvs_get_blob(h, NVS_BLOB, &s_cfg, &len) != ESP_OK || len != sizeof(s_cfg))
        memset(&s_cfg, 0, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = 0;
    nvs_close(h);
}

static void nvs_save_snapshot(const countdown_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_BLOB, cfg, sizeof(*cfg));
    nvs_commit(h);
    nvs_close(h);
}

/* Helpers */

static int days_remaining(const countdown_item_t *item)
{
    struct tm now_tm;
    if (!time_sync_get_local_relaxed(&now_tm)) return -1;

    struct tm target = {0};
    target.tm_year = item->year - 1900;
    target.tm_mon  = item->month - 1;
    target.tm_mday = item->day;
    target.tm_hour = 0;

    time_t t_now    = mktime(&now_tm);
    time_t t_target = mktime(&target);

    now_tm.tm_hour = 0;
    now_tm.tm_min  = 0;
    now_tm.tm_sec  = 0;
    t_now = mktime(&now_tm);

    double diff = difftime(t_target, t_now);
    return (int)(diff / 86400.0);
}

/* EPD render */

/** YYYY-MM-DD into buf[11]; clamps fields so no truncation warnings. */
static void format_date_ymd(char buf[11], int y, int mo, int da)
{
    if (y < 0) y = 0;
    else if (y > 9999) y = 9999;
    if (mo < 1) mo = 1;
    else if (mo > 12) mo = 12;
    if (da < 1) da = 1;
    else if (da > 31) da = 31;
    buf[0] = (char)('0' + (y / 1000) % 10);
    buf[1] = (char)('0' + (y / 100) % 10);
    buf[2] = (char)('0' + (y / 10) % 10);
    buf[3] = (char)('0' + y % 10);
    buf[4] = '-';
    buf[5] = (char)('0' + mo / 10);
    buf[6] = (char)('0' + mo % 10);
    buf[7] = '-';
    buf[8] = (char)('0' + da / 10);
    buf[9] = (char)('0' + da % 10);
    buf[10] = '\0';
}

static void draw_header(fb_t *fb, int W)
{
    char today[16] = "";
    struct tm now;
    if (time_sync_get_local_relaxed(&now)) {
        format_date_ymd(today, now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    }
    (void)W;
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6", today, true);
}

static const char *status_text_for_days(int days)
{
    if (days > 0)
        return "\xe8\xbf\x98\xe6\x9c\x89";
    if (days == 0)
        return "\xe4\xbb\x8a\xe5\xa4\xa9";
    return "\xe5\xb7\xb2\xe8\xbf\x87";
}

static const char *hero_badge_text_for_days(int days)
{
    if (days > 0)
        return "\xe6\x9c\xaa\xe5\x88\xb0\xe6\x9c\x9f";
    if (days == 0)
        return "\xe4\xbb\x8a\xe5\xa4\xa9";
    return "\xe5\xb7\xb2\xe8\xbf\x87\xe6\x9c\x9f";
}

static const char *suffix_text_for_days(int days)
{
    if (days > 0)
        return "\xe5\xa4\xa9\xe5\x90\x8e";
    if (days == 0)
        return "\xe7\x9b\xae\xe6\xa0\x87\xe6\x97\xa5\xe5\xb7\xb2\xe5\x88\xb0";
    return "\xe5\xa4\xa9\xe5\x89\x8d";
}

static int countdown_sort_score(int days)
{
    if (days >= 0)
        return days;
    return 10000 - days;
}

static int cd_text_width_px(const char *s, int px)
{
    if (!s)
        return 0;
    return ui_text_width_px(NULL, s, px);
}

static int cd_ascii_width_builtin(const char *s, int scale)
{
    if (!s)
        return 0;
    if (scale < 1)
        scale = 1;
    return (int)strlen(s) * 8 * scale;
}

static int cd_fit_px(const char *s, int max_w, int max_h, int preferred_px)
{
    static const int choices[] = { 72, 64, 56, 48, 40, 32, 24, 16 };
    if (preferred_px > 72)
        preferred_px = 72;
    if (preferred_px < 16)
        preferred_px = 16;
    for (int i = 0; i < (int)(sizeof(choices) / sizeof(choices[0])); i++) {
        int px = choices[i];
        if (px > preferred_px)
            continue;
        if ((max_w <= 0 || cd_text_width_px(s, px) <= max_w) &&
            (max_h <= 0 || px <= max_h)) {
            return px;
        }
    }
    return 16;
}

static int cd_draw_centered_px(fb_t *fb, int x, int y, int w,
                               const char *s, fb_color_t color, int px)
{
    int tw = cd_text_width_px(s, px);
    int tx = x + (w - tw) / 2;
    if (tx < x)
        tx = x;
    if (px == 16)
        return ui_draw_fixed_text_maxw(fb, tx, y, s, color, 1,
                                       w - (tx - x));
    return ui_draw_text_px_maxw(fb, tx, y, s, color, px, w - (tx - x));
}

static int cd_top_for_midline(int mid_y, int px)
{
    return mid_y - px / 2;
}

static int cd_draw_ascii_midline_builtin(fb_t *fb, int x, int mid_y,
                                         const char *s, fb_color_t color,
                                         int scale)
{
    if (scale < 1)
        scale = 1;
    return ui_draw_fixed_text(fb, x, mid_y - 8 * scale, s, color, scale);
}

static int cd_draw_ascii_midline_builtin_maxw(fb_t *fb, int x, int mid_y,
                                              const char *s, fb_color_t color,
                                              int scale, int max_w)
{
    if (scale < 1)
        scale = 1;
    return ui_draw_fixed_text_maxw(fb, x, mid_y - 8 * scale,
                                   s, color, scale, max_w);
}

static int cd_draw_midline_px(fb_t *fb, int x, int mid_y, const char *s,
                              fb_color_t color, int px)
{
    if (px == 16)
        return ui_draw_fixed_text(fb, x, cd_top_for_midline(mid_y, px),
                                  s, color, 1);
    return ui_draw_text_px(fb, x, cd_top_for_midline(mid_y, px), s, color, px);
}

static int cd_draw_midline_px_maxw(fb_t *fb, int x, int mid_y, const char *s,
                                   fb_color_t color, int px, int max_w)
{
    if (px == 16)
        return ui_draw_fixed_text_maxw(fb, x, cd_top_for_midline(mid_y, px),
                                       s, color, 1, max_w);
    return ui_draw_text_px_maxw(fb, x, cd_top_for_midline(mid_y, px), s,
                                color, px, max_w);
}

static void sort_active_indices(int *idx, int n, const countdown_config_t *cfg)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int di = days_remaining(&cfg->items[idx[i]]);
            int dj = days_remaining(&cfg->items[idx[j]]);
            if (countdown_sort_score(dj) < countdown_sort_score(di)) {
                int tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
        }
    }
}

static void draw_day_metric(fb_t *fb, int left_x, int right_x, int center_y,
                            int days, int card_h)
{
    int abs_days = days < 0 ? -days : days;
    fb_color_t c = (days >= 0) ? COLOR_RED : COLOR_BLACK;
    int max_w = right_x - left_x;
    if (max_w < 16)
        max_w = 16;

    if (days == 0) {
        const char *today = "\xe4\xbb\x8a\xe5\xa4\xa9";
        int px = cd_fit_px(today, max_w, card_h - 10, card_h >= 70 ? 32 : 24);
        cd_draw_midline_px_maxw(fb, right_x - cd_text_width_px(today, px),
                                center_y, today, COLOR_RED, px, max_w);
        return;
    }

    char days_str[16];
    snprintf(days_str, sizeof(days_str), "%d", abs_days);
    const char *unit = suffix_text_for_days(days);
    char full[32];
    snprintf(full, sizeof(full), "%s%s", days_str, unit);
    int text_px = 16;
    int text_w = cd_text_width_px(full, text_px);

    if (text_w <= max_w) {
        int x = right_x - text_w;
        if (x < left_x)
            x = left_x;
        cd_draw_midline_px_maxw(fb, x, center_y, full, c, text_px, max_w);
        return;
    }

    int block_h = text_px * 2 + 3;
    int top = center_y - block_h / 2;
    if (top < center_y - card_h / 2 + 4)
        top = center_y - card_h / 2 + 4;
    cd_draw_centered_px(fb, left_x, top, max_w, days_str, c, text_px);
    cd_draw_centered_px(fb, left_x, top + text_px + 3, max_w, unit, c, text_px);
}

static void draw_hero(fb_t *fb, int W, int H, const countdown_item_t *item, int days)
{
    const int s = ui_scale_for(fb);
    const bool is_583 = ui_layout_is_583(fb);
    const bool large = (ui_layout_for(fb) == UI_LAYOUT_LARGE);
    const bool wide_layout = is_583 || large;
    const int margin = is_583 ? 34 : 14 * s;
    const int card_x = margin;
    const int card_y = is_583 ? 74 : 38 * s;
    const int card_w = W - 2 * margin;
    int card_h = H - card_y - 34 * s;
    if (card_h < 160)
        card_h = H - card_y - 24 * s;

    char days_str[16];
    int abs_days = days < 0 ? -days : days;
    snprintf(days_str, sizeof(days_str), "%d", abs_days);

    bool hot = (days >= 0 && days <= 7);
    fb_color_t accent = hot ? COLOR_RED : COLOR_BLACK;
    ui_draw_card(fb, card_x, card_y, card_w, card_h, hot);
    ui_draw_section_label(fb, card_x + 12 * s, card_y + 10 * s,
                          "\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6\xe7\x9b\xae\xe6\xa0\x87",
                          accent, 1);
    {
        const char *status = hero_badge_text_for_days(days);
        int status_w = cd_text_width_px(status, 16);
        cd_draw_midline_px_maxw(fb, card_x + card_w - 12 * s - status_w,
                                card_y + 18 * s, status, accent, 16,
                                status_w);
    }

    int title_px = is_583 ? 40 : (wide_layout ? 32 : 24);
    int title_max = card_w - 28 * s;
    title_px = cd_fit_px(item->title, title_max, title_px, title_px);
    int title_y = card_y + (is_583 ? 58 : 36 * s);
    int title_w = cd_text_width_px(item->title, title_px);
    if (title_w <= title_max) {
        ui_draw_text_px(fb, card_x + (card_w - title_w) / 2, title_y,
                        item->title, COLOR_BLACK, title_px);
    } else {
        ui_draw_text_px_maxw(fb, card_x + 14 * s, title_y, item->title,
                             COLOR_BLACK, title_px, title_max);
    }

    char date_buf[11];
    format_date_ymd(date_buf, item->year, item->month, item->day);
    int date_y = card_y + card_h - 26 * s;
    ui_draw_dotted_hline(fb, card_x + 12 * s, date_y - 8 * s,
                         card_w - 24 * s, accent, 6);
    cd_draw_midline_px(fb, card_x + 14 * s, date_y + 8,
                       "\xe7\x9b\xae\xe6\xa0\x87\xe6\x97\xa5", COLOR_BLACK, 16);
    cd_draw_ascii_midline_builtin(
        fb, card_x + card_w - 14 * s - cd_ascii_width_builtin(date_buf, 1),
        date_y + 8, date_buf, COLOR_BLACK, 1);

    int num_top = title_y + title_px + 12 * s;
    int num_bottom = date_y - 12 * s;
    if (days == 0) {
        const char *primary = "\xe4\xbb\x8a\xe5\xa4\xa9";
        const char *sub = "\xe7\x9b\xae\xe6\xa0\x87\xe6\x97\xa5\xe5\xb7\xb2\xe5\x88\xb0";
        int primary_px = 32;
        int sub_px = (ui_layout_is_42(fb) || wide_layout) ? 24 : 16;
        primary_px = cd_fit_px(primary, card_w - 32 * s,
                               num_bottom - num_top - 6 * s - sub_px,
                               32);
        int primary_w = cd_text_width_px(primary, primary_px);
        int block_h = primary_px + 6 * s + sub_px;
        int block_y = num_top + ((num_bottom - num_top) - block_h) / 2;
        if (block_y < num_top)
            block_y = num_top;
        cd_draw_midline_px_maxw(fb, card_x + (card_w - primary_w) / 2,
                                block_y + primary_px / 2, primary,
                                COLOR_RED, primary_px, card_w - 32 * s);
        int sub_w = cd_text_width_px(sub, sub_px);
        cd_draw_midline_px(fb, card_x + (card_w - sub_w) / 2,
                           block_y + primary_px + 6 * s + sub_px / 2,
                           sub, COLOR_RED, sub_px);
    } else {
        const char *prefix = (days > 0) ? "\xe8\xbf\x98\xe6\x9c\x89" : "\xe5\xb7\xb2\xe8\xbf\x87";
        const char *unit = "\xe5\xa4\xa9";
        fb_color_t line_c = (days > 0) ? COLOR_RED : COLOR_BLACK;
        int prefix_px = 24;
        int num_px = is_583 ? 72 : (ui_layout_is_42(fb) ? 32 : (wide_layout ? 48 : 40));
        int unit_px = 24;
        int gap2 = 8 * s;
        int prefix_w;
        int num_w;
        int unit_w;
        int total_w;
        for (;;) {
            prefix_w = cd_text_width_px(prefix, prefix_px);
            num_w = cd_text_width_px(days_str, num_px);
            unit_w = cd_text_width_px(unit, unit_px);
            total_w = prefix_w + gap2 + num_w + gap2 + unit_w;
            if (total_w <= card_w - 32 * s || num_px <= 32)
                break;
            num_px -= 8;
        }
        if (total_w > card_w - 32 * s) {
            prefix_px = 16;
            unit_px = 16;
            gap2 = 6 * s;
            prefix_w = cd_text_width_px(prefix, prefix_px);
            num_w = cd_text_width_px(days_str, num_px);
            unit_w = cd_text_width_px(unit, unit_px);
            total_w = prefix_w + gap2 + num_w + gap2 + unit_w;
        }

        int line_mid = num_top + (num_bottom - num_top) / 2;
        int x = card_x + (card_w - total_w) / 2;
        if (x < card_x + 16 * s)
            x = card_x + 16 * s;
        int small_mid = line_mid + (num_px - prefix_px) / 2 - 2;
        cd_draw_midline_px(fb, x, small_mid, prefix, line_c, prefix_px);
        x += prefix_w + gap2;
        cd_draw_midline_px(fb, x, line_mid, days_str, line_c, num_px);
        x += num_w + gap2;
        small_mid = line_mid + (num_px - unit_px) / 2 - 2;
        cd_draw_midline_px(fb, x, small_mid, unit, line_c, unit_px);
    }
}

static void draw_cards(fb_t *fb, int W, int H,
                       const countdown_config_t *cfg, int active_count)
{
    (void)active_count;
    const int s = ui_scale_for(fb);
    const bool is_583 = ui_layout_is_583(fb);
    const bool large = (ui_layout_for(fb) == UI_LAYOUT_LARGE);
    const bool wide_layout = is_583 || large;
    const int body_y = is_583 ? 74 : 38 * s;
    const int margin = is_583 ? 34 : 14 * s;
    const int gap = is_583 ? 12 : 7 * s;

    int active_idx[COUNTDOWN_MAX_ITEMS];
    int n = 0;
    for (int i = 0; i < cfg->count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (cfg->items[i].active) active_idx[n++] = i;
    }
    if (n <= 0) return;
    sort_active_indices(active_idx, n, cfg);

    int pages = (n + 3) / 4;
    if (pages < 1) pages = 1;
    int page = (int)(s_page_idx % pages);
    int start = page * 4;
    int show_n = n - start;
    if (show_n > 4) show_n = 4;
    if (show_n < 1) show_n = 1;

    ui_draw_section_label(fb, margin, body_y,
                          "\xe8\xbf\x91\xe6\x9c\x9f\xe4\xba\x8b\xe4\xbb\xb6",
                          COLOR_BLACK, 1);

    int list_y = body_y + 22 * s;
    int body_h = H - list_y - 34 * s;
    int card_h = (body_h - gap * (show_n - 1)) / show_n;
    if (card_h > 88) card_h = 88;
    if (card_h < 42) card_h = 42;

    int right_col = is_583 ? 190 : (wide_layout ? 172 : (ui_layout_is_42(fb) ? 126 : 116));
    int left_maxw = W - margin * 2 - right_col - 12 * s;
    if (left_maxw < 80) left_maxw = 80;

    int y = list_y;
    for (int k = 0; k < show_n; k++) {
        int i = active_idx[start + k];
        int days = days_remaining(&cfg->items[i]);
        bool hot = (days >= 0 && days <= 7);
        fb_color_t accent = hot ? COLOR_RED : COLOR_BLACK;

        ui_draw_card(fb, margin, y, W - margin * 2, card_h, hot);
        fb_fill_rect(fb, margin + 1, y + 8, 3 * s, card_h - 16, accent);

        bool title_builtin = ui_layout_is_42(fb);
        int title_px = title_builtin ? 16 : ((is_583 && card_h >= 58) ? 32 : (wide_layout && card_h >= 58 ? 24 : 16));
        if (!title_builtin)
            title_px = cd_fit_px(cfg->items[i].title, left_maxw, 24, title_px);
        int title_y = y + (title_builtin ? 10 * s : 8 * s);
        int text_x = margin + 10 * s;
        if (title_builtin) {
            ui_draw_fixed_text_maxw(fb, text_x, title_y,
                                    cfg->items[i].title, COLOR_BLACK, 1,
                                    left_maxw);
        } else {
            ui_draw_text_px_maxw(fb, text_x, title_y, cfg->items[i].title,
                                 COLOR_BLACK, title_px, left_maxw);
        }

        char date_buf[11];
        format_date_ymd(date_buf, cfg->items[i].year,
                        cfg->items[i].month, cfg->items[i].day);
        int meta_mid = title_y + title_px + 5 * s + 8;
        if (meta_mid + 8 > y + card_h - 6)
            meta_mid = y + card_h - 14;

        const char *status = status_text_for_days(days);
        cd_draw_midline_px(fb, text_x, meta_mid, status, accent, 16);
        int status_w = cd_text_width_px(status, 16);
        cd_draw_ascii_midline_builtin_maxw(fb, text_x + status_w + 8 * s,
                                           meta_mid, date_buf, COLOR_BLACK, 1,
                                           left_maxw - status_w - 8 * s);

        int sep_x = W - margin - right_col + 4 * s;
        ui_draw_dotted_vline(fb, sep_x, y + 10, card_h - 20,
                             COLOR_BLACK, 6);
        draw_day_metric(fb, sep_x + 8 * s, W - margin - 12 * s,
                        y + card_h / 2, days, card_h);

        y += card_h + gap;
    }

    char page_str[16];
    if (pages > 1)
        snprintf(page_str, sizeof(page_str), "%d/%d", page + 1, pages);
    else
        snprintf(page_str, sizeof(page_str), "%d \xe9\xa1\xb9", n);
    ui_draw_footer(fb, "\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6", page_str);
}

esp_err_t countdown_show(void)
{
    countdown_config_t cfg;
    esp_err_t cfg_err = countdown_get_config(&cfg);
    if (cfg_err != ESP_OK)
        return cfg_err;

    if (!cfg.enabled) {
        ESP_LOGW(TAG, "Countdown disabled");
        return ESP_ERR_INVALID_STATE;
    }

    unsigned epoch = display_policy_display_epoch();

    fb_t *fb = fb_create();
    if (!fb) return ESP_ERR_NO_MEM;
    fb_clear(fb);

    int W = epd_width(), H = epd_height();

    int active_count = 0;
    int first_active = -1;
    for (int i = 0; i < cfg.count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (cfg.items[i].active) {
            if (first_active < 0) first_active = i;
            active_count++;
        }
    }

    draw_header(fb, W);

    if (active_count == 0) {
        ui_draw_empty_state(fb,
                            "\xe6\x9a\x82\xe6\x97\xa0\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6",
                            "\xe8\xaf\xb7\xe9\x80\x9a\xe8\xbf\x87\xe7\xbd\x91\xe9\xa1\xb5\xe6\xb7\xbb\xe5\x8a\xa0");
        ui_draw_footer(fb, "\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6", "\xe6\x97\xa0\xe4\xba\x8b\xe9\xa1\xb9");
    } else if (active_count == 1) {
        int days = days_remaining(&cfg.items[first_active]);
        draw_hero(fb, W, H, &cfg.items[first_active], days);
        ui_draw_footer(fb, "\xe5\x80\x92\xe8\xae\xa1\xe6\x97\xb6",
                       days == 0 ? "\xe4\xbb\x8a\xe5\xa4\xa9" : "\xe9\x87\x8d\xe7\x82\xb9");
    } else {
        draw_cards(fb, W, H, &cfg, active_count);
        if (active_count > 4) {
            int pages = (active_count + 3) / 4;
            s_page_idx = (uint8_t)((s_page_idx + 1) % pages);
        } else {
            s_page_idx = 0;
        }
    }

    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Countdown display skipped: stale request");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t exp_err = fb_export(fb, "/spiffs/image.bin");
    fb_destroy(fb);
    if (exp_err != ESP_OK) {
        fb_raw_file_unlock();
        ESP_LOGE(TAG, "fb_export failed: %s", esp_err_to_name(exp_err));
        return exp_err;
    }
    if (!display_policy_epoch_is_current(epoch)) {
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Countdown display skipped before EPD refresh");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t disp_err = epd_display_from_file("/spiffs/image.bin");
    fb_raw_file_unlock();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(disp_err));
        return disp_err;
    }

    display_policy_set_manual_screen_active(true);
    scheduler_notify_manual_show();

    ESP_LOGI(TAG, "Countdown displayed (%d active items)", active_count);
    return ESP_OK;
}

/* Public API */

esp_err_t countdown_init(void)
{
    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex)
        return ESP_ERR_NO_MEM;
    nvs_load();
    ESP_LOGI(TAG, "init ok (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}

esp_err_t countdown_get_config(countdown_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_cfg_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

esp_err_t countdown_set_config(const countdown_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_cfg_mutex) return ESP_ERR_INVALID_STATE;
    countdown_config_t snap;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = COUNTDOWN_MAX_ITEMS;
    snap = s_cfg;
    xSemaphoreGive(s_cfg_mutex);

    nvs_save_snapshot(&snap);
    ESP_LOGI(TAG, "Config updated (enabled=%d, items=%d)", snap.enabled, snap.count);
    return ESP_OK;
}
