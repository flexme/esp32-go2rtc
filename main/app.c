#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"
#include "xrtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

xrtc_status_t g_status;
static const char *TAG = "xrtc";
static bool ap_enabled;
static unsigned disconnects;
void xrtc_phase(const char *phase, const char *error) {
    strlcpy(g_status.phase, phase, sizeof(g_status.phase));
    strlcpy(g_status.error, error ? error : "", sizeof(g_status.error));
    ESP_LOGI(TAG, "phase=%s%s%s", g_status.phase, error ? " error=" : "", error ? error : "");
}
static void event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_status.wifi = false;
        g_status.ip[0] = 0;
        disconnects++;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(g_status.ip, sizeof(g_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        g_status.wifi = true;
        disconnects = 0;
        if (ap_enabled) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            ap_enabled = false;
        }
        xrtc_phase("ready", NULL);
        ESP_LOGI(TAG, "Web UI http://%s/", g_status.ip);
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        static bool time_started;
        if (!time_started) {
            esp_netif_sntp_init(&sntp);
            time_started = true;
        }
    }
}
void network_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL));
    xrtc_config_t cfg;
    config_get(&cfg);
    wifi_config_t station = {0};
    esp_wifi_get_config(WIFI_IF_STA, &station);
    if (cfg.ssid[0]) {
        memset(&station, 0, sizeof(station));
        strlcpy((char *)station.sta.ssid, cfg.ssid, sizeof(station.sta.ssid));
        strlcpy((char *)station.sta.password, cfg.wifi_password, sizeof(station.sta.password));
    }
    wifi_config_t ap = {0};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "xiaomi-rtc-%02x%02x%02x", mac[3], mac[4],
             mac[5]);
    memcpy(ap.ap.password, cfg.admin_token, 16);
    ap.ap.password[16] = 0;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(station.sta.ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (station.sta.ssid[0])
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    printf("XRTC_SETUP_AP=%s PASSWORD=%.*s\n", ap.ap.ssid, 16, cfg.admin_token);
    sodium_memzero(&cfg, sizeof(cfg));
    sodium_memzero(&station, sizeof(station));
    sodium_memzero(&ap, sizeof(ap));
}
static void console_task(void *arg) {
    char *line = malloc(8192);
    size_t used = 0;
    if (!line)
        vTaskDelete(NULL);
    for (;;) {
        char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!used)
                continue;
            line[used] = 0;
            cJSON *j = cJSON_Parse(line);
            used = 0;
            const char *op = json_string(j, "op");
            if (!strcmp(op, "provision")) {
                cJSON *cfg = cJSON_GetObjectItemCaseSensitive(j, "config");
                esp_err_t error = cJSON_IsObject(cfg) ? config_update(cfg) : ESP_ERR_INVALID_ARG;
                cJSON *epoch = cJSON_GetObjectItemCaseSensitive(j, "epoch");
                if (cJSON_IsNumber(epoch) && epoch->valuedouble > 1700000000) {
                    struct timeval tv = {.tv_sec = (time_t)epoch->valuedouble};
                    settimeofday(&tv, NULL);
                }
                printf("XRTC_PROVISION_RESULT=%s\n", error == ESP_OK ? "ok" : "invalid");
                fflush(stdout);
                cJSON *restart = cJSON_GetObjectItemCaseSensitive(j, "restart");
                bool reboot = error == ESP_OK && cJSON_IsTrue(restart);
                cJSON_Delete(j);
                sodium_memzero(line, 8192);
                if (reboot) {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
                continue;
            } else if (!strcmp(op, "status"))
                printf("XRTC_STATUS phase=%s ip=%s frames=%llu rtp=%llu\n", g_status.phase,
                       g_status.ip, (unsigned long long)g_status.frames,
                       (unsigned long long)g_status.rtp_packets);
            else if (!strcmp(op, "restart")) {
                cJSON_Delete(j);
                esp_restart();
            } else if (!strcmp(op, "stop"))
                rtc_stop();
            cJSON_Delete(j);
            sodium_memzero(line, 8192);
        } else if (used < 8191)
            line[used++] = c;
        else {
            used = 0;
            sodium_memzero(line, 8192);
        }
    }
}
void app_main(void) {
    if (sodium_init() < 0)
        abort();
    ESP_LOGI(TAG, "xiaomi-rtc %s PSRAM=%u", XRTC_VERSION,
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "PSRAM is required");
        return;
    }
    config_init();
    cloud_init();
    xrtc_peer_init();
    miss_init();
    network_init();
    web_start();
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    xTaskCreate(console_task, "setup_console", 6144, NULL, 2, NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "wifi=%d viewer=%d camera=%d frames=%llu rtp=%llu internal=%u psram=%u",
                 g_status.wifi, g_status.viewer, g_status.camera,
                 (unsigned long long)g_status.frames, (unsigned long long)g_status.rtp_packets,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
