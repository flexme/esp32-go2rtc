#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "xrtc.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static xrtc_config_t cfg;
static SemaphoreHandle_t lock;
static nvs_handle_t nvs;

void config_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(nvs_open("xiaomi_rtc", NVS_READWRITE, &nvs));
    size_t n = sizeof(cfg);
    if (nvs_get_blob(nvs, "config", &cfg, &n) != ESP_OK || n != sizeof(cfg) || cfg.version != 1) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.version = 1;
        cfg.quality = 1;
        strcpy(cfg.region, "cn");
    }
    if (!cfg.admin_token[0]) {
        uint8_t random[24];
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); i++)
            sprintf(cfg.admin_token + i * 2, "%02x", random[i]);
        ESP_ERROR_CHECK(nvs_set_blob(nvs, "config", &cfg, sizeof(cfg)));
        ESP_ERROR_CHECK(nvs_commit(nvs));
    }
    // Only the physical serial console discloses the setup credential.
    printf("XRTC_SETUP_TOKEN=%s\n", cfg.admin_token);
}
void config_get(xrtc_config_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cfg;
    xSemaphoreGive(lock);
}
static bool safe_text(const char *s, size_t cap) {
    if (strlen(s) >= cap)
        return false;
    for (; *s; s++)
        if ((unsigned char)*s < 32 || *s == 127)
            return false;
    return true;
}
esp_err_t config_update(const cJSON *json) {
    xSemaphoreTake(lock, portMAX_DELAY);
    xrtc_config_t next = cfg;
    struct {
        const char *name;
        char *dest;
        size_t capacity;
    } fields[] = {
        {"ssid", next.ssid, sizeof(next.ssid)},
        {"wifi_password", next.wifi_password, sizeof(next.wifi_password)},
        {"region", next.region, sizeof(next.region)},
        {"camera_ip", next.camera_ip, sizeof(next.camera_ip)},
        {"did", next.did, sizeof(next.did)},
        {"model", next.model, sizeof(next.model)},
        {"user_id", next.user_id, sizeof(next.user_id)},
        {"pass_token", next.pass_token, sizeof(next.pass_token)},
    };
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(json, fields[i].name);
        if (!value)
            continue;
        if (!cJSON_IsString(value) || !safe_text(value->valuestring, fields[i].capacity)) {
            result = ESP_ERR_INVALID_ARG;
            break;
        }
        strcpy(fields[i].dest, value->valuestring);
    }
    const char *regions[] = {"cn", "de", "i2", "ru", "sg", "us"};
    bool region_ok = false;
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        if (!strcmp(next.region, regions[i]))
            region_ok = true;
    if (!region_ok)
        result = ESP_ERR_INVALID_ARG;
    if (next.camera_ip[0]) {
        struct in_addr addr;
        uint32_t ip;
        if (!inet_aton(next.camera_ip, &addr))
            result = ESP_ERR_INVALID_ARG;
        else {
            ip = ntohl(addr.s_addr);
            if (!((ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8))
                result = ESP_ERR_INVALID_ARG;
        }
    }
    for (const char *p = next.did; *p; p++)
        if (!isdigit((unsigned char)*p))
            result = ESP_ERR_INVALID_ARG;
    for (const char *p = next.user_id; *p; p++)
        if (!isdigit((unsigned char)*p))
            result = ESP_ERR_INVALID_ARG;
    cJSON *quality = cJSON_GetObjectItemCaseSensitive(json, "quality");
    if (quality) {
        if (!cJSON_IsNumber(quality) || (quality->valuedouble != 1 && quality->valuedouble != 2))
            result = ESP_ERR_INVALID_ARG;
        else
            next.quality = (uint8_t)quality->valueint;
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(nvs, "config", &next, sizeof(next));
        if (result == ESP_OK)
            result = nvs_commit(nvs);
        if (result == ESP_OK)
            cfg = next;
    }
    memset(&next, 0, sizeof(next));
    xSemaphoreGive(lock);
    return result;
}
cJSON *config_public(void) {
    xrtc_config_t current;
    config_get(&current);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "ssid", current.ssid);
    cJSON_AddStringToObject(out, "region", current.region);
    cJSON_AddStringToObject(out, "camera_ip", current.camera_ip);
    cJSON_AddStringToObject(out, "did", current.did);
    cJSON_AddStringToObject(out, "model", current.model);
    cJSON_AddNumberToObject(out, "quality", current.quality);
    cJSON_AddBoolToObject(out, "account_configured", current.user_id[0] && current.pass_token[0]);
    memset(&current, 0, sizeof(current));
    return out;
}
