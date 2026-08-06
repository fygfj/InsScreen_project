#include "http_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"
#include "music_player.h"
#include "sd_card.h"
#include "speaker_test.h"

static const char *TAG = "http_music";

#define MUSIC_UPLOAD_MAX_BYTES   (32U * 1024U * 1024U)
#define MUSIC_UPLOAD_CHUNK_BYTES 4096U

static void music_send_result(httpd_req_t *req, esp_err_t err)
{
    char json[128];
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return;
    }

    const char *status = "500 Internal Server Error";
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE ||
        err == ESP_ERR_NOT_SUPPORTED)
        status = "400 Bad Request";
    else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_TIMEOUT)
        status = "409 Conflict";
    else if (err == ESP_ERR_NOT_FOUND)
        status = "404 Not Found";

    snprintf(json, sizeof(json), "{\"ok\":false,\"err\":\"%s\"}",
             esp_err_to_name(err));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void music_send_text_error(httpd_req_t *req, const char *status,
                                  const char *err_text)
{
    char esc[80];
    char json[128];
    json_escape(esc, sizeof(esc), err_text ? err_text : "error");
    httpd_resp_set_status(req, status ? status : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    snprintf(json, sizeof(json), "{\"ok\":false,\"err\":\"%s\"}", esc);
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static int music_hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void music_url_decode_in_place(char *s)
{
    if (!s)
        return;

    char *dst = s;
    for (char *src = s; *src; src++) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = music_hex_value(src[1]);
            int lo = music_hex_value(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 2;
                continue;
            }
        }
        *dst++ = (*src == '+') ? ' ' : *src;
    }
    *dst = '\0';
}

static esp_err_t music_receive_file(httpd_req_t *req, FILE *f, size_t *written)
{
    uint8_t buf[MUSIC_UPLOAD_CHUNK_BYTES];
    size_t remain = (size_t)req->content_len;
    size_t total = 0;

    while (remain > 0) {
        size_t want = remain > sizeof(buf) ? sizeof(buf) : remain;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0)
            return ESP_FAIL;

        size_t w = fwrite(buf, 1, (size_t)r, f);
        if (w != (size_t)r)
            return ESP_FAIL;
        total += w;
        remain -= (size_t)r;
    }

    if (written)
        *written = total;
    return ferror(f) ? ESP_FAIL : ESP_OK;
}

static bool music_read_json_name(httpd_req_t *req, char *name, size_t name_len,
                                 uint8_t *volume)
{
    if (!req || !name || name_len == 0)
        return false;
    name[0] = '\0';

    if (http_get_query_param(req, "name", name, name_len))
        return name[0] != '\0';

    if (req->content_len <= 0)
        return false;

    char body[256] = {0};
    if (!http_read_request_body(req, body, sizeof(body), "request body too large"))
        return false;

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return false;

    const char *jname = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    if (jname)
        snprintf(name, name_len, "%s", jname);

    cJSON *jv = cJSON_GetObjectItem(root, "volume");
    if (volume && jv && cJSON_IsNumber(jv) && jv->valueint >= 1 && jv->valueint <= 100)
        *volume = (uint8_t)jv->valueint;

    cJSON_Delete(root);
    return name[0] != '\0';
}

esp_err_t music_list_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) {
        music_send_result(req, err);
        return ESP_OK;
    }

    DIR *dir = opendir(SD_CARD_MUSIC_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed: %s", SD_CARD_MUSIC_DIR);
        music_send_result(req, ESP_ERR_NOT_FOUND);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"tracks\":[");
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!music_player_is_supported_file(ent->d_name))
            continue;

        size_t name_len = strlen(ent->d_name);
        if (name_len >= 96)
            continue;
        char name[96];
        memcpy(name, ent->d_name, name_len + 1);

        char path[192];
        snprintf(path, sizeof(path), "%s/%s", SD_CARD_MUSIC_DIR, name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        char esc[128];
        json_escape(esc, sizeof(esc), name);
        char item[224];
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"bytes\":%llu}",
                 first ? "" : ",", esc, (unsigned long long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

esp_err_t music_status_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    music_player_status_t st;
    music_player_get_status(&st);

    char track[128];
    char err[48];
    json_escape(track, sizeof(track), st.track);
    json_escape(err, sizeof(err), st.last_error);

    char json[512];
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"track\":\"%s\",\"volume\":%u,"
             "\"sample_rate\":%lu,\"channels\":%u,\"bits\":%u,"
             "\"elapsed_ms\":%lu,\"data_bytes\":%llu,"
             "\"data_played\":%llu,\"last_error\":\"%s\"}",
             music_player_state_name(st.state), track,
             (unsigned)st.volume_percent,
             (unsigned long)st.sample_rate_hz,
             (unsigned)st.channels,
             (unsigned)st.bits_per_sample,
             (unsigned long)st.elapsed_ms,
             (unsigned long long)st.data_bytes_total,
             (unsigned long long)st.data_bytes_played,
             err);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t music_play_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    char name[96];
    uint8_t volume = 0;
    if (!music_read_json_name(req, name, sizeof(name), &volume)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"missing name\"}");
        return ESP_OK;
    }

    if (volume > 0)
        (void)music_player_set_volume(volume);
    music_send_result(req, music_player_play(name));
    return ESP_OK;
}

esp_err_t music_pause_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;
    music_send_result(req, music_player_pause());
    return ESP_OK;
}

esp_err_t music_resume_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;
    music_send_result(req, music_player_resume());
    return ESP_OK;
}

esp_err_t music_stop_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;
    music_send_result(req, music_player_stop());
    return ESP_OK;
}

esp_err_t music_volume_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    char body[96] = {0};
    if (!http_read_request_body(req, body, sizeof(body), "request body too large"))
        return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"invalid json\"}");
        return ESP_OK;
    }
    cJSON *jv = cJSON_GetObjectItem(root, "volume");
    int volume = (jv && cJSON_IsNumber(jv)) ? jv->valueint : 0;
    cJSON_Delete(root);

    if (volume < 1 || volume > 100) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"invalid volume\"}");
        return ESP_OK;
    }

    music_send_result(req, music_player_set_volume((uint8_t)volume));
    return ESP_OK;
}

esp_err_t music_upload_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    char name[96];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        music_send_text_error(req, "400 Bad Request", "missing name");
        return ESP_OK;
    }
    music_url_decode_in_place(name);

    if (!music_player_is_supported_file(name)) {
        music_send_text_error(req, "400 Bad Request", "invalid music filename");
        return ESP_OK;
    }
    if (req->content_len <= 0) {
        music_send_text_error(req, "400 Bad Request", "empty upload");
        return ESP_OK;
    }
    if ((size_t)req->content_len > MUSIC_UPLOAD_MAX_BYTES) {
        music_send_text_error(req, "413 Payload Too Large", "music file too large");
        return ESP_OK;
    }

    (void)music_player_stop();

    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) {
        music_send_result(req, err);
        return ESP_OK;
    }

    char final_path[192];
    char tmp_path[208];
    snprintf(final_path, sizeof(final_path), "%s/%s", SD_CARD_MUSIC_DIR, name);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.uploading", SD_CARD_MUSIC_DIR, name);
    remove(tmp_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open upload failed: %s errno=%d", tmp_path, errno);
        music_send_result(req, ESP_FAIL);
        return ESP_OK;
    }

    size_t written = 0;
    err = music_receive_file(req, f, &written);
    int close_err = fclose(f);
    if (err != ESP_OK || close_err != 0) {
        ESP_LOGW(TAG, "music upload write failed: %s err=%s errno=%d",
                 tmp_path, esp_err_to_name(err), errno);
        remove(tmp_path);
        music_send_result(req, ESP_FAIL);
        return ESP_OK;
    }

    err = music_player_validate_file(tmp_path, name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "music upload invalid audio: %s %s",
                 tmp_path, esp_err_to_name(err));
        remove(tmp_path);
        music_send_result(req, err);
        return ESP_OK;
    }

    struct stat old_st;
    if (stat(final_path, &old_st) == 0 && remove(final_path) != 0) {
        ESP_LOGW(TAG, "remove old music failed: %s errno=%d", final_path, errno);
        remove(tmp_path);
        music_send_result(req, ESP_FAIL);
        return ESP_OK;
    }
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGW(TAG, "rename music failed: %s -> %s errno=%d",
                 tmp_path, final_path, errno);
        remove(tmp_path);
        music_send_result(req, ESP_FAIL);
        return ESP_OK;
    }

    char esc[128];
    char json[192];
    json_escape(esc, sizeof(esc), name);
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"name\":\"%s\",\"bytes\":%llu}",
             esc, (unsigned long long)written);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t speaker_test_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req))
        return ESP_OK;

    int volume = 35;
    if (req->content_len > 0) {
        char body[96] = {0};
        if (!http_read_request_body(req, body, sizeof(body), "request body too large"))
            return ESP_OK;

        cJSON *root = cJSON_Parse(body);
        if (!root) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"invalid json\"}");
            return ESP_OK;
        }
        cJSON *jv = cJSON_GetObjectItem(root, "volume");
        if (jv && cJSON_IsNumber(jv))
            volume = jv->valueint;
        cJSON_Delete(root);
    }

    if (volume < 1)
        volume = 1;
    if (volume > 100)
        volume = 100;

    (void)music_player_stop();
    (void)music_player_set_volume((uint8_t)volume);
    music_send_result(req, speaker_test_play_tone((uint8_t)volume));
    return ESP_OK;
}
