#include "news_feed.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "display_mode.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "scheduler.h"
#include "sensor_local.h"
#include "ui_theme.h"

static const char *TAG = "news";
static const char *NVS_NS = "news";

#define NEWS_DEFAULT_REFRESH_SEC 1800
#define NEWS_MIN_REFRESH_SEC     60
#define NEWS_MAX_REFRESH_SEC     86400
#define NEWS_HTTP_TIMEOUT_MS     12000
#define NEWS_HTTP_MAX_BYTES      12288
#define NEWS_DEFAULT_PAGE_SIZE   NEWS_FEED_MAX_ITEMS
#define NEWS_JUHE_ENDPOINT       "https://v.juhe.cn/toutiao/index"

typedef struct {
    char *buf;
    int   len;
    int   cap;
    bool  overflow;
} news_resp_buf_t;

static news_feed_config_t s_cfg = {
    .enabled = false,
    .api_key = "",
    .category = "top",
    .page_size = NEWS_DEFAULT_PAGE_SIZE,
    .source_url = "",
    .refresh_sec = NEWS_DEFAULT_REFRESH_SEC,
};

static news_feed_data_t s_data = {
    .valid = false,
    .page_title = "热点资讯",
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_render_mutex;
static SemaphoreHandle_t s_fetch_mutex;
static SemaphoreHandle_t s_task_wakeup;
static atomic_bool       s_pending;
static atomic_uint       s_pending_epoch;
static atomic_int        s_pending_flags;
static int64_t           s_last_fetch_us;

static void normalize_source_url(char *url, size_t url_len);
static bool news_api_key_valid(const char *key);
static bool news_category_valid(const char *category);
static bool news_url_query_value(const char *url, const char *name,
                                 char *out, size_t out_len);
static esp_err_t news_prepare_config(news_feed_config_t *cfg);
static int news_items_per_page(int width, int height);

enum {
    NEWS_RENDER_ADVANCE     = 1 << 0,
    NEWS_RENDER_FORCE_FETCH = 1 << 1,
};

static uint32_t clamp_refresh_sec(uint32_t sec)
{
    if (sec < NEWS_MIN_REFRESH_SEC) return NEWS_MIN_REFRESH_SEC;
    if (sec > NEWS_MAX_REFRESH_SEC) return NEWS_MAX_REFRESH_SEC;
    return sec;
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t enabled = 0;
    if (nvs_get_u8(h, "enabled", &enabled) == ESP_OK)
        s_cfg.enabled = enabled != 0;

    size_t len = sizeof(s_cfg.source_url);
    nvs_get_str(h, "url", s_cfg.source_url, &len);

    len = sizeof(s_cfg.api_key);
    nvs_get_str(h, "key", s_cfg.api_key, &len);

    len = sizeof(s_cfg.category);
    nvs_get_str(h, "category", s_cfg.category, &len);

    uint8_t page_size = 0;
    if (nvs_get_u8(h, "page_size", &page_size) == ESP_OK)
        s_cfg.page_size = page_size;

    uint32_t refresh = 0;
    if (nvs_get_u32(h, "refresh", &refresh) == ESP_OK)
        s_cfg.refresh_sec = clamp_refresh_sec(refresh);

    nvs_close(h);

    /*
     * 兼容旧版：以前网页只保存一条完整 URL。如果 URL 是聚合新闻接口，
     * 第一次启动新固件时自动提取 key、type 和 page_size，用户不用重填。
     */
    if (!s_cfg.api_key[0] && s_cfg.source_url[0]) {
        char value[NEWS_FEED_API_KEY_LEN];
        if (news_url_query_value(s_cfg.source_url, "key", value, sizeof(value)))
            snprintf(s_cfg.api_key, sizeof(s_cfg.api_key), "%s", value);
        (void)news_url_query_value(s_cfg.source_url, "type",
                                   s_cfg.category, sizeof(s_cfg.category));
        if (news_url_query_value(s_cfg.source_url, "page_size", value, sizeof(value))) {
            unsigned long n = strtoul(value, NULL, 10);
            if (n > 0 && n <= NEWS_FEED_MAX_ITEMS)
                s_cfg.page_size = (uint8_t)n;
        }
    }

    (void)news_prepare_config(&s_cfg);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "enabled", s_cfg.enabled ? 1 : 0);
    nvs_set_str(h, "key", s_cfg.api_key);
    nvs_set_str(h, "category", s_cfg.category);
    nvs_set_u8(h, "page_size", s_cfg.page_size);
    nvs_set_str(h, "url", s_cfg.source_url);
    nvs_set_u32(h, "refresh", s_cfg.refresh_sec);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0)
        return ESP_OK;

    news_resp_buf_t *rb = (news_resp_buf_t *)evt->user_data;
    if (rb->len + evt->data_len >= rb->cap) {
        int room = rb->cap - rb->len - 1;
        if (room > 0) {
            memcpy(rb->buf + rb->len, evt->data, room);
            rb->len += room;
            rb->buf[rb->len] = '\0';
        }
        rb->overflow = true;
        return ESP_FAIL;
    }

    memcpy(rb->buf + rb->len, evt->data, evt->data_len);
    rb->len += evt->data_len;
    rb->buf[rb->len] = '\0';
    return ESP_OK;
}

static bool url_looks_http(const char *url)
{
    return url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static void normalize_source_url(char *url, size_t url_len)
{
    if (!url || url_len == 0) return;

    char *start = url;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != url)
        memmove(url, start, strlen(start) + 1);

    size_t len = strlen(url);
    while (len > 0 && (url[len - 1] == ' ' || url[len - 1] == '\t' ||
                       url[len - 1] == '\r' || url[len - 1] == '\n')) {
        url[--len] = '\0';
    }

    if (url[0] == '\0' || url_looks_http(url))
        return;
    if (strncmp(url, "//", 2) == 0) {
        char tmp[NEWS_FEED_MAX_URL];
        snprintf(tmp, sizeof(tmp), "https:%s", url);
        snprintf(url, url_len, "%s", tmp);
        return;
    }

    char tmp[NEWS_FEED_MAX_URL];
    snprintf(tmp, sizeof(tmp), "https://%s", url);
    snprintf(url, url_len, "%s", tmp);
}

static bool news_api_key_valid(const char *key)
{
    if (!key || !key[0])
        return false;

    /*
     * 聚合数据的 Key 通常是数字和英文字母。额外允许 '-'、'_'，但不允许
     * '&'、'?' 等字符进入查询串，避免用户误粘贴整条 URL。
     */
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++, len++) {
        bool ok = (*p >= '0' && *p <= '9') ||
                  (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_';
        if (!ok || len + 1 >= NEWS_FEED_API_KEY_LEN)
            return false;
    }
    return len >= 8;
}

static bool news_category_valid(const char *category)
{
    static const char *const allowed[] = {
        "top", "guonei", "guoji", "yule", "tiyu", "junshi",
        "keji", "caijing", "youxi", "qiche", "jiankang",
    };
    if (!category || !category[0])
        return false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(category, allowed[i]) == 0)
            return true;
    }
    return false;
}

static bool news_url_query_value(const char *url, const char *name,
                                 char *out, size_t out_len)
{
    if (!url || !name || !name[0] || !out || out_len == 0)
        return false;

    const char *p = strchr(url, '?');
    if (!p)
        return false;
    p++;

    size_t name_len = strlen(name);
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == name_len &&
            strncmp(p, name, name_len) == 0) {
            size_t value_len = (size_t)(end - eq - 1);
            if (value_len == 0 || value_len >= out_len)
                return false;
            memcpy(out, eq + 1, value_len);
            out[value_len] = '\0';
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t news_prepare_config(news_feed_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;

    cfg->api_key[sizeof(cfg->api_key) - 1] = '\0';
    cfg->category[sizeof(cfg->category) - 1] = '\0';
    cfg->source_url[sizeof(cfg->source_url) - 1] = '\0';

    if (!news_category_valid(cfg->category))
        snprintf(cfg->category, sizeof(cfg->category), "top");
    if (cfg->page_size < 1 || cfg->page_size > NEWS_FEED_MAX_ITEMS)
        cfg->page_size = NEWS_DEFAULT_PAGE_SIZE;

    if (cfg->api_key[0]) {
        if (!news_api_key_valid(cfg->api_key))
            return ESP_ERR_INVALID_ARG;

        int n = snprintf(cfg->source_url, sizeof(cfg->source_url),
                         NEWS_JUHE_ENDPOINT
                         "?key=%s&type=%s&page=1&page_size=%u&is_filter=1",
                         cfg->api_key, cfg->category,
                         (unsigned)cfg->page_size);
        if (n < 0 || n >= (int)sizeof(cfg->source_url))
            return ESP_ERR_INVALID_SIZE;
        return ESP_OK;
    }

    /* 没有新字段时保留旧版自定义 JSON URL 的兼容能力。 */
    normalize_source_url(cfg->source_url, sizeof(cfg->source_url));
    return cfg->source_url[0] ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static char *news_http_get(const char *url)
{
    if (!url_looks_http(url))
        return NULL;

    news_resp_buf_t rb = {
        .buf = malloc(NEWS_HTTP_MAX_BYTES),
        .len = 0,
        .cap = NEWS_HTTP_MAX_BYTES,
        .overflow = false,
    };
    if (!rb.buf)
        return NULL;
    rb.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = NEWS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(rb.buf);
        return NULL;
    }
    esp_http_client_set_header(client, "Accept", "application/json,text/plain,*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || rb.overflow || rb.len == 0) {
        ESP_LOGW(TAG, "HTTP GET failed: err=%s status=%d len=%d overflow=%d",
                 esp_err_to_name(err), status, rb.len, rb.overflow ? 1 : 0);
        free(rb.buf);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP GET OK (%d bytes)", rb.len);
    return rb.buf;
}

static void clean_text(char *s)
{
    if (!s) return;

    char *r = s;
    char *w = s;
    bool in_tag = false;
    bool last_space = false;

    while (*r) {
        unsigned char c = (unsigned char)*r++;
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (in_tag) {
            if (c == '>')
                in_tag = false;
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (last_space)
                continue;
            last_space = true;
        } else {
            last_space = false;
        }
        *w++ = (char)c;
    }
    while (w > s && w[-1] == ' ')
        w--;
    *w = '\0';
}

static const char *json_string_any(cJSON *obj, const char *a, const char *b, const char *c)
{
    const char *v = NULL;
    if (a) v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, a));
    if (!v && b) v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, b));
    if (!v && c) v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, c));
    return v;
}

static bool parse_news_json(const char *json, news_feed_data_t *out)
{
    if (!json || !out) return false;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;

    news_feed_data_t data = {0};
    snprintf(data.page_title, sizeof(data.page_title), "热点资讯");

    cJSON *arr = NULL;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else {
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
        const char *updated = json_string_any(root, "updated_at", "update_time", "time");
        if (title && title[0])
            snprintf(data.page_title, sizeof(data.page_title), "%s", title);
        if (updated && updated[0])
            snprintf(data.updated_at, sizeof(data.updated_at), "%s", updated);
        arr = cJSON_GetObjectItem(root, "items");
        if (!cJSON_IsArray(arr))
            arr = cJSON_GetObjectItem(root, "news");
        if (!cJSON_IsArray(arr))
            arr = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsArray(arr)) {
            cJSON *result = cJSON_GetObjectItem(root, "result");
            if (cJSON_IsObject(result)) {
                arr = cJSON_GetObjectItem(result, "data");
                if (!updated)
                    updated = cJSON_GetStringValue(cJSON_GetObjectItem(result, "date"));
                if (updated && updated[0])
                    snprintf(data.updated_at, sizeof(data.updated_at), "%s", updated);
            }
        }
    }

    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(arr);
    if (count > NEWS_FEED_MAX_ITEMS)
        count = NEWS_FEED_MAX_ITEMS;

    for (int i = 0; i < count; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        news_feed_item_t *dst = &data.items[data.item_count];

        if (cJSON_IsString(it)) {
            snprintf(dst->title, sizeof(dst->title), "%s", it->valuestring);
        } else if (cJSON_IsObject(it)) {
            const char *title = json_string_any(it, "title", "headline", "name");
            const char *summary = json_string_any(it, "summary", "description", "desc");
            if (!summary)
                summary = cJSON_GetStringValue(cJSON_GetObjectItem(it, "content"));
            const char *source = json_string_any(it, "source", "site", "author");
            const char *category = cJSON_GetStringValue(cJSON_GetObjectItem(it, "category"));
            const char *time = json_string_any(it, "time", "date", "published_at");

            cJSON *source_obj = cJSON_GetObjectItem(it, "source");
            if (!source && cJSON_IsObject(source_obj))
                source = cJSON_GetStringValue(cJSON_GetObjectItem(source_obj, "name"));
            if (!source)
                source = cJSON_GetStringValue(cJSON_GetObjectItem(it, "author_name"));

            if (title)
                snprintf(dst->title, sizeof(dst->title), "%s", title);
            if (summary)
                snprintf(dst->summary, sizeof(dst->summary), "%s", summary);
            else if (category || source || time)
                snprintf(dst->summary, sizeof(dst->summary), "%s | %s | %s",
                         category ? category : "-",
                         source ? source : "-",
                         time ? time : "-");
            if (source)
                snprintf(dst->source, sizeof(dst->source), "%s", source);
            if (time)
                snprintf(dst->time, sizeof(dst->time), "%s", time);
        }

        clean_text(dst->title);
        clean_text(dst->summary);
        clean_text(dst->source);
        clean_text(dst->time);

        if (dst->title[0])
            data.item_count++;
    }

    cJSON_Delete(root);

    if (data.item_count <= 0)
        return false;
    if (!data.updated_at[0] && data.items[0].time[0])
        snprintf(data.updated_at, sizeof(data.updated_at), "%s", data.items[0].time);
    data.valid = true;
    data.current_index = 0;
    *out = data;
    return true;
}

static int utf8_decode(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    if (!p || p[0] == 0) return -1;
    if (p[0] < 0x80) { *pp += 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *pp += 2;
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp += 3;
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *pp += 4;
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
               ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *pp += 1;
    return -1;
}

static int utf8_encode_codepoint(int cp, char out[5])
{
    if (cp < 0) cp = '?';
    if (cp < 0x80) {
        out[0] = (char)cp; out[1] = '\0'; return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0'; return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0'; return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
    return 4;
}

typedef struct {
    const char *start;
    int len;
    int width;
} news_line_t;

static int wrap_text(const char *text, int px, int max_w, news_line_t *lines, int max_lines)
{
    if (!text || !lines || max_lines <= 0)
        return 0;

    const char *p = text;
    const char *line_start = p;
    int cur_w = 0;
    int count = 0;

    while (*p && count < max_lines) {
        if (*p == '\n') {
            lines[count++] = (news_line_t){ line_start, (int)(p - line_start), cur_w };
            p++;
            line_start = p;
            cur_w = 0;
            continue;
        }

        const char *next = p;
        int cp = utf8_decode(&next);
        if (cp < 0) {
            p = next;
            continue;
        }
        char one[5];
        utf8_encode_codepoint(cp, one);
        int cw = ui_text_width_px(NULL, one, px);

        if (cur_w + cw > max_w && cur_w > 0) {
            lines[count++] = (news_line_t){ line_start, (int)(p - line_start), cur_w };
            line_start = p;
            cur_w = 0;
            if (count >= max_lines)
                break;
        }

        cur_w += cw;
        p = next;
    }

    if (count < max_lines && (cur_w > 0 || p > line_start))
        lines[count++] = (news_line_t){ line_start, (int)(p - line_start), cur_w };
    return count;
}

static void draw_wrapped(fb_t *fb, int x, int y, int max_w, const char *text,
                         int px, int line_h, int max_lines, fb_color_t color)
{
    news_line_t lines[8];
    int n = wrap_text(text, px, max_w, lines, max_lines);
    for (int i = 0; i < n; i++) {
        char buf[NEWS_FEED_SUMMARY_LEN];
        int len = lines[i].len;
        if (len >= (int)sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, lines[i].start, len);
        buf[len] = '\0';
        ui_draw_text_px(fb, x, y + i * line_h, buf, color, px);
    }
}

static void render_current(unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        return;
    }

    news_feed_config_t cfg;
    news_feed_data_t data;
    portENTER_CRITICAL(&s_mux);
    cfg = s_cfg;
    data = s_data;
    portEXIT_CRITICAL(&s_mux);

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);

    char right[32] = {0};
    sensor_local_data_t sensor = {0};
    if (sensor_local_ensure_fresh(SENSOR_LOCAL_DISPLAY_MAX_AGE_MS) == ESP_OK &&
        sensor_local_get_data(&sensor) == ESP_OK &&
        sensor.enabled && sensor.present && sensor.valid &&
        sensor.age_ms >= 0 && sensor.age_ms <= SENSOR_LOCAL_DISPLAY_MAX_AGE_MS) {
        snprintf(right, sizeof(right), "室内 %.1fC %.0f%%",
                 sensor.temperature_c, sensor.humidity_percent);
    } else if (data.updated_at[0]) {
        size_t n = strlen(data.updated_at);
        if (n >= 5)
            snprintf(right, sizeof(right), "%.5s", data.updated_at + (n >= 16 ? 5 : 0));
        else
            snprintf(right, sizeof(right), "%.15s", data.updated_at);
    }
    ui_draw_header(fb, data.page_title[0] ? data.page_title : "热点资讯",
                   right[0] ? right : "", true);

    const int s = ui_scale_for(fb);

    if (!cfg.enabled) {
        ui_draw_empty_state(fb, "新闻未开启", "请在网页端开启新闻功能");
    } else if (!data.valid || data.item_count <= 0) {
        ui_draw_empty_state(fb, "暂无新闻", "请检查新闻源或网络");
    } else {
        int idx = data.current_index;
        if (idx < 0 || idx >= data.item_count)
            idx = 0;

        const bool is_42 = ui_layout_is_42(fb);
        const bool wide = ui_layout_is_wide(fb);
        const int per_page = news_items_per_page(fb->width, fb->height);
        const int end = (idx + per_page < data.item_count)
                            ? idx + per_page : data.item_count;
        const int shown = end - idx;
        const int content_top = is_42 ? 38 : 30 * s;
        const int content_bottom = is_42 ? fb->height - 42
                                         : fb->height - 28 * s;
        const int row_h = (content_bottom - content_top) / shown;
        const int accent_x = is_42 ? 16 : 12 * s;
        const int text_x = is_42 ? 30 : 22 * s;
        const int max_w = fb->width - text_x - (is_42 ? 14 : 12 * s);
        const int title_px = is_42 ? 18 : (wide ? 26 : 14);
        const int title_line_h = title_px + (wide ? 7 : 4);
        const int meta_px = is_42 ? 14 : (wide ? 18 : 12);

        for (int i = 0; i < shown; i++) {
            const int item_index = idx + i;
            const int row_y = content_top + i * row_h;
            const news_feed_item_t *it = &data.items[item_index];

            /* 红色短竖线代替大图标，既区分条目，又减少墨水屏红色刷新面积。 */
            fb_fill_rect(fb, accent_x, row_y + 5, is_42 ? 3 : 2 * s,
                         row_h > 18 ? row_h - 14 : 4, COLOR_RED);
            draw_wrapped(fb, text_x, row_y + 2, max_w, it->title,
                         title_px, title_line_h, 2, COLOR_BLACK);

            const char *short_time = it->time;
            if (short_time[0] && strlen(short_time) >= 16)
                short_time += 5; /* 2026-07-22 21:12:00 -> 07-22 21:12:00 */
            char meta[112];
            snprintf(meta, sizeof(meta), "%d  %s · %.11s",
                     item_index + 1,
                     it->source[0] ? it->source : "未知来源",
                     short_time[0] ? short_time : "时间未知");
            int meta_y = row_y + 2 * title_line_h + (wide ? 7 : 2);
            ui_draw_text_px_maxw(fb, text_x, meta_y, meta,
                                 COLOR_BLACK, meta_px, max_w);

            if (i + 1 < shown) {
                int line_y = row_y + row_h - (wide ? 5 : 3);
                ui_draw_dotted_hline(fb, text_x, line_y, max_w,
                                     COLOR_BLACK, wide ? 10 : 7);
            }
        }

        char page[40];
        snprintf(page, sizeof(page), "第 %d-%d / %d 条",
                 idx + 1, end, data.item_count);
        ui_draw_footer(fb, "聚合新闻 · 自动更新", page);
    }

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale news render");
        return;
    }

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK && epd_is_ready() && display_policy_epoch_is_current(epoch)) {
        esp_err_t disp_err = epd_display_from_file(raw_path);
        if (disp_err == ESP_OK) {
            display_policy_set_manual_screen_active(true);
            scheduler_notify_manual_show();
            ESP_LOGI(TAG, "News displayed");
        } else {
            ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(disp_err));
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "fb_export failed: %s", esp_err_to_name(err));
    }
    fb_raw_file_unlock();
}

esp_err_t news_feed_fetch_now(void)
{
    if (!s_fetch_mutex)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_fetch_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    news_feed_config_t cfg;
    portENTER_CRITICAL(&s_mux);
    cfg = s_cfg;
    portEXIT_CRITICAL(&s_mux);

    if (!cfg.enabled || !url_looks_http(cfg.source_url)) {
        xSemaphoreGive(s_fetch_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    char *json = news_http_get(cfg.source_url);
    if (!json) {
        xSemaphoreGive(s_fetch_mutex);
        return ESP_FAIL;
    }

    news_feed_data_t parsed;
    bool ok = parse_news_json(json, &parsed);
    free(json);
    if (!ok) {
        xSemaphoreGive(s_fetch_mutex);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_mux);
    /*
     * 获取到新内容后保留当前页。否则每次联网都会把索引重置为 0，
     * 自动轮播将永远只能看到第一页。
     */
    int per_page = news_items_per_page(epd_width(), epd_height());
    int old_page = s_data.current_index / per_page;
    int page_count = (parsed.item_count + per_page - 1) / per_page;
    if (old_page >= page_count)
        old_page = 0;
    parsed.current_index = old_page * per_page;
    s_data = parsed;
    s_last_fetch_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);

    xSemaphoreGive(s_fetch_mutex);
    ESP_LOGI(TAG, "Fetched %d news items", parsed.item_count);
    return ESP_OK;
}

static bool data_needs_fetch(const news_feed_config_t *cfg)
{
    if (!cfg || !cfg->enabled || !url_looks_http(cfg->source_url))
        return false;

    news_feed_data_t data;
    int64_t last;
    portENTER_CRITICAL(&s_mux);
    data = s_data;
    last = s_last_fetch_us;
    portEXIT_CRITICAL(&s_mux);

    if (!data.valid || data.item_count <= 0)
        return true;
    int64_t age_us = esp_timer_get_time() - last;
    return age_us > (int64_t)cfg->refresh_sec * 1000000LL;
}

static int news_items_per_page(int width, int height)
{
    /*
     * 400x300 及更大的常用墨水屏每页放 3 条；窄长小屏放 2 条；
     * 200x200 一类的小屏只放 1 条，保证标题仍然看得清。
     */
    if (width >= 400 && height >= 272)
        return 3;
    if (height >= 260)
        return 2;
    return 1;
}

static void advance_index(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_data.valid && s_data.item_count > 0) {
        int per_page = news_items_per_page(epd_width(), epd_height());
        int page_count = (s_data.item_count + per_page - 1) / per_page;
        int page = s_data.current_index / per_page;
        s_data.current_index = ((page + 1) % page_count) * per_page;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void render_task(void *arg)
{
    unsigned epoch = (unsigned)(uintptr_t)arg;
    int flags = atomic_exchange(&s_pending_flags, 0);

    for (;;) {
        atomic_store(&s_pending, false);

        news_feed_config_t cfg;
        portENTER_CRITICAL(&s_mux);
        cfg = s_cfg;
        portEXIT_CRITICAL(&s_mux);

        if ((flags & NEWS_RENDER_FORCE_FETCH) || data_needs_fetch(&cfg))
            (void)news_feed_fetch_now();
        if (flags & NEWS_RENDER_ADVANCE)
            advance_index();

        render_current(epoch);

        if (!atomic_load(&s_pending))
            break;
        epoch = atomic_load(&s_pending_epoch);
        flags = atomic_exchange(&s_pending_flags, 0);
    }

    xSemaphoreGive(s_render_mutex);
    vTaskDelete(NULL);
}

static esp_err_t queue_render(int flags, unsigned *out_epoch)
{
    if (!s_render_mutex)
        return ESP_ERR_INVALID_STATE;

    unsigned epoch = display_policy_begin_manual_display();
    if (out_epoch)
        *out_epoch = epoch;

    if (xSemaphoreTake(s_render_mutex, 0) != pdTRUE) {
        atomic_store(&s_pending_epoch, epoch);
        atomic_fetch_or(&s_pending_flags, flags);
        atomic_store(&s_pending, true);
        return ESP_OK;
    }

    atomic_store(&s_pending_flags, flags);
    BaseType_t ret = xTaskCreate(render_task, "news_render", 10240,
                                 (void *)(uintptr_t)epoch, 5, NULL);
    if (ret != pdPASS) {
        xSemaphoreGive(s_render_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t news_feed_show(void)
{
    return queue_render(0, NULL);
}

esp_err_t news_feed_show_next(void)
{
    return queue_render(NEWS_RENDER_ADVANCE, NULL);
}

esp_err_t news_feed_refresh_and_show(void)
{
    return queue_render(NEWS_RENDER_FORCE_FETCH, NULL);
}

static void news_task(void *arg)
{
    (void)arg;
    for (;;) {
        news_feed_config_t cfg;
        portENTER_CRITICAL(&s_mux);
        cfg = s_cfg;
        portEXIT_CRITICAL(&s_mux);

        uint32_t wait_ms = 10000;
        if (cfg.enabled && cfg.refresh_sec > 0)
            wait_ms = cfg.refresh_sec * 1000UL;

        if (s_task_wakeup)
            xSemaphoreTake(s_task_wakeup, pdMS_TO_TICKS(wait_ms));
        else
            vTaskDelay(pdMS_TO_TICKS(wait_ms));

        portENTER_CRITICAL(&s_mux);
        cfg = s_cfg;
        portEXIT_CRITICAL(&s_mux);
        if (!cfg.enabled)
            continue;

        if (data_needs_fetch(&cfg))
            (void)news_feed_fetch_now();

        if (display_mode_active() == DISPLAY_MODE_NEWS)
            (void)news_feed_show_next();
    }
}

esp_err_t news_feed_init(void)
{
    nvs_load();
    s_cfg.refresh_sec = clamp_refresh_sec(s_cfg.refresh_sec);

    s_render_mutex = xSemaphoreCreateBinary();
    s_fetch_mutex = xSemaphoreCreateMutex();
    s_task_wakeup = xSemaphoreCreateBinary();
    if (!s_render_mutex || !s_fetch_mutex || !s_task_wakeup)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_render_mutex);

    BaseType_t ret = xTaskCreate(news_task, "news_task", 6144, NULL, 3, NULL);
    if (ret != pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG,
             "News init: enabled=%d refresh=%lus category=%s count=%u provider=%s",
             s_cfg.enabled ? 1 : 0, (unsigned long)s_cfg.refresh_sec,
             s_cfg.category, (unsigned)s_cfg.page_size,
             s_cfg.api_key[0] ? "juhe" : "legacy_url");
    return ESP_OK;
}

esp_err_t news_feed_get_config(news_feed_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t news_feed_set_config(const news_feed_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    news_feed_config_t next = *cfg;
    next.refresh_sec = clamp_refresh_sec(next.refresh_sec);
    esp_err_t prep_err = news_prepare_config(&next);
    if (prep_err != ESP_OK &&
        (next.enabled || prep_err != ESP_ERR_INVALID_STATE)) {
        return prep_err;
    }

    portENTER_CRITICAL(&s_mux);
    s_cfg = next;
    if (!next.enabled) {
        s_data.valid = false;
        s_data.item_count = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    nvs_save();

    if (s_task_wakeup)
        xSemaphoreGive(s_task_wakeup);
    return ESP_OK;
}

bool news_feed_config_ready(void)
{
    news_feed_config_t cfg;
    news_feed_get_config(&cfg);
    return cfg.enabled && url_looks_http(cfg.source_url);
}

void news_feed_get_data_copy(news_feed_data_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_data;
    portEXIT_CRITICAL(&s_mux);
}
