#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "metrics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"
#include "xrtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
extern const uint8_t diagnostics_start[] asm("_binary_diagnostics_js_start");
extern const uint8_t diagnostics_end[] asm("_binary_diagnostics_js_end");
static bool authorized(httpd_req_t *req) {
    char header[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        return false;
    xrtc_config_t cfg;
    config_get(&cfg);
    bool ok = strlen(header) == 7 + strlen(cfg.admin_token) && !strncmp(header, "Bearer ", 7) &&
              sodium_memcmp(header + 7, cfg.admin_token, strlen(cfg.admin_token)) == 0;
    sodium_memzero(&cfg, sizeof(cfg));
    return ok;
}
static esp_err_t respond(httpd_req_t *req, cJSON *o) {
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t result = httpd_resp_send(req, text ? text : "{}", HTTPD_RESP_USE_STRLEN);
    free(text);
    return result;
}
static cJSON *result(bool ok, const char *error) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (error)
        cJSON_AddStringToObject(o, "error", error);
    return o;
}
static cJSON *body(httpd_req_t *req) {
    if (req->content_len < 2 || req->content_len > 24576)
        return NULL;
    char *text = malloc(req->content_len + 1);
    if (!text)
        return NULL;
    size_t used = 0;
    int timeouts = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, text + used, req->content_len - used);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
            continue;
        if (n <= 0) {
            sodium_memzero(text, req->content_len);
            free(text);
            return NULL;
        }
        used += n;
    }
    text[used] = 0;
    cJSON *j = cJSON_ParseWithLength(text, used);
    sodium_memzero(text, used);
    free(text);
    return j;
}
static esp_err_t root(httpd_req_t *req) {
    if (!strcmp(req->uri, "/diagnostics.js")) {
        httpd_resp_set_type(req, "text/javascript; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)diagnostics_start, diagnostics_end - diagnostics_start - 1);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(
        req, "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data:; media-src 'self' blob:; frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)html_start, html_end - html_start - 1);
}
static esp_err_t api(httpd_req_t *req) {
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return respond(req, result(false, "Enter the setup token from the serial console"));
    }
    const char *path = req->uri;
    cJSON *j = NULL, *out = NULL;
    if (req->method == HTTP_POST) {
        j = body(req);
        if (!j)
            return respond(req, result(false, "Invalid JSON request"));
    }
    if (!strcmp(path, "/api/status")) {
        out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "version", XRTC_VERSION);
        cJSON_AddStringToObject(out, "phase", g_status.phase);
        cJSON_AddStringToObject(out, "error", g_status.error);
        cJSON_AddStringToObject(out, "ip", g_status.ip);
        cJSON_AddBoolToObject(out, "wifi", g_status.wifi);
        cJSON_AddBoolToObject(out, "camera", g_status.camera);
        cJSON_AddBoolToObject(out, "viewer", g_status.viewer);
        cJSON_AddNumberToObject(out, "frames", g_status.frames);
        cJSON_AddNumberToObject(out, "video_bytes", g_status.video_bytes);
        cJSON_AddNumberToObject(out, "rtp_packets", g_status.rtp_packets);
        cJSON_AddNumberToObject(out, "dropped_frames", g_status.dropped_frames);
        cJSON_AddNumberToObject(out, "reconnects", g_status.reconnects);
        cJSON_AddNumberToObject(out, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(out, "internal_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(out, "internal_min_since_boot", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "internal_largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "uptime_ms", esp_timer_get_time() / 1000.0);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            cJSON_AddNumberToObject(out, "wifi_rssi_dbm", ap.rssi);
        cJSON_AddItemToObject(out, "media", metrics_status());
    } else if (!strcmp(path, "/api/clock")) {
        out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "mono_ms", esp_timer_get_time() / 1000.0);
    } else if (!strcmp(path, "/api/rtc/timings")) {
        out = metrics_timings();
        if (!out)
            out = result(false, "Timing snapshot allocation failed");
    } else if (!strcmp(path, "/api/config")) {
        if (req->method == HTTP_GET)
            out = config_public();
        else if (rtc_wanted())
            out = result(false, "Stop playback before changing configuration");
        else
            out = result(config_update(j) == ESP_OK, NULL);
    } else if (!strcmp(path, "/api/account"))
        out = rtc_wanted() ? result(false, "Stop playback before signing in") : cloud_action(j);
    else if (!strcmp(path, "/api/devices"))
        out = cloud_devices();
    else if (!strcmp(path, "/api/time")) {
        cJSON *epoch = cJSON_GetObjectItemCaseSensitive(j, "epoch");
        if (cJSON_IsNumber(epoch) && epoch->valuedouble > 1700000000 &&
            epoch->valuedouble < 4102444800) {
            struct timeval tv = {.tv_sec = (time_t)epoch->valuedouble};
            settimeofday(&tv, NULL);
            out = result(true, NULL);
        } else
            out = result(false, "Invalid clock value");
    } else if (!strcmp(path, "/api/rtc/offer")) {
        uint32_t session;
        char *offer = g_status.wifi ? rtc_offer(&session) : NULL;
        if (offer) {
            out = result(true, NULL);
            cJSON_AddStringToObject(out, "sdp", offer);
            cJSON_AddNumberToObject(out, "session", session);
            free(offer);
        } else
            out = result(false, "Camera is busy, or Wi-Fi is not connected");
    } else if (!strcmp(path, "/api/rtc/answer")) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "session");
        out = result(cJSON_IsNumber(s) &&
                         rtc_answer((uint32_t)s->valuedouble, json_string(j, "sdp")) == ESP_OK,
                     NULL);
    } else if (!strcmp(path, "/api/rtc/heartbeat")) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "session");
        if (cJSON_IsNumber(s))
            rtc_heartbeat((uint32_t)s->valuedouble);
        out = result(true, NULL);
    } else if (!strcmp(path, "/api/rtc/stop")) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "session");
        if (s) {
            if (cJSON_IsNumber(s) && s->valuedouble > 0 && s->valuedouble <= UINT32_MAX && s->valuedouble == (uint32_t)s->valuedouble)
                rtc_stop_session((uint32_t)s->valuedouble);
        } else rtc_stop();
        out = result(true, NULL);
    } else if (!strcmp(path, "/api/restart")) {
        out = result(true, NULL);
    } else
        out = result(false, "Unknown endpoint");
    cJSON_Delete(j);
    esp_err_t rc = respond(req, out);
    if (!strcmp(path, "/api/restart")) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return rc;
}
void web_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 28;
    config.stack_size = 24576;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = root};
    httpd_register_uri_handler(server, &page);
    page.uri = "/diagnostics.js";
    httpd_register_uri_handler(server, &page);
    const char *get[] = {"/api/status", "/api/config", "/api/devices", "/api/clock", "/api/rtc/timings"};
    const char *post[] = {"/api/config",    "/api/account",    "/api/time",
                          "/api/rtc/offer", "/api/rtc/answer", "/api/rtc/heartbeat",
                          "/api/rtc/stop",  "/api/restart"};
    for (size_t i = 0; i < sizeof(get) / sizeof(get[0]); i++) {
        httpd_uri_t route = {.uri = get[i], .method = HTTP_GET, .handler = api};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
    for (size_t i = 0; i < sizeof(post)/sizeof(post[0]); i++) {
        httpd_uri_t route = {.uri = post[i], .method = HTTP_POST, .handler = api};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
}
