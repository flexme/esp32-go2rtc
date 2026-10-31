#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XRTC_VERSION "0.1.0"
#define XRTC_MAX_FRAME (512 * 1024)
typedef struct {
    uint32_t version;
    char ssid[33], wifi_password[65];
    char region[4], camera_ip[16], did[32], model[96];
    char user_id[32], pass_token[1024], admin_token[49];
    uint8_t quality;
} xrtc_config_t;
typedef struct {
    char phase[48], error[160], ip[16];
    bool wifi, camera, viewer;
    uint64_t video_bytes, frames, rtp_packets, dropped_frames;
    uint32_t reconnects;
} xrtc_status_t;

extern xrtc_status_t g_status;
void xrtc_phase(const char *phase, const char *error);
void config_init(void);
void config_get(xrtc_config_t *out);
esp_err_t config_update(const cJSON *json);
cJSON *config_public(void);
void network_init(void);
void web_start(void);
void miss_init(void);
void miss_set_wanted(bool wanted);
void miss_restart(void);
void xrtc_peer_init(void);
char *rtc_offer(uint32_t *session);
esp_err_t rtc_answer(uint32_t session, const char *sdp);
void rtc_stop(void);
void rtc_stop_session(uint32_t session);
void rtc_heartbeat(uint32_t session);
bool rtc_wanted(void);
bool rtc_connected(void);
void rtc_video(const uint8_t *data, size_t size, uint64_t timestamp_ms, int64_t received_us);

typedef struct {
    uint8_t public_key[32], secret_key[32], shared_key[32];
    char public_hex[65], sign[2048];
} camera_auth_t;
void cloud_init(void);
esp_err_t cloud_camera_auth(camera_auth_t *auth);
cJSON *cloud_action(const cJSON *request);
cJSON *cloud_devices(void);

const char *json_string(const cJSON *o, const char *key);
char *url_encode(const char *s);
char *b64_encode(const uint8_t *data, size_t size);
uint8_t *b64_decode(const char *s, size_t *size);
void hash_bytes(const char *algorithm, const void *data, size_t size, uint8_t *out);
void rc4_drop1024(const uint8_t key[32], const uint8_t *input, uint8_t *output, size_t size);
