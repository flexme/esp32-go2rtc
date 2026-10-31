#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hevc.h"
#include "metrics.h"
#include "peer.h"
#include "peer_connection.h"
#include "xrtc.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static SemaphoreHandle_t mutex;
static PeerConnection *peer;
static uint32_t generation;
static int64_t lease, created_at;
static hevc_sender_t *sender;
static atomic_bool connected;
static atomic_bool resync_needed;
static void request_keyframe(void *ctx) {
    metrics_pli();
    if (sender)
        sender->waiting_keyframe = true;
}
static void changed(PeerConnectionState state, void *ctx) {
    connected = state == PEER_CONNECTION_CONNECTED;
    g_status.viewer = connected;
    if (connected) {
        xrtc_phase("viewer_connected", NULL);
        if (sender)
            sender->waiting_keyframe = true;
        miss_set_wanted(true);
    } else if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_CLOSED ||
               state == PEER_CONNECTION_DISCONNECTED)
        miss_set_wanted(false);
}
static void stop_locked(void) {
    miss_set_wanted(false);
    connected = false;
    g_status.viewer = false;
    if (peer) {
        peer_connection_close(peer);
        peer_connection_destroy(peer);
        peer = NULL;
    }
    if (sender)
        hevc_init(sender, esp_random(), esp_random());
    atomic_store(&resync_needed, false);
    xrtc_phase(g_status.wifi ? "ready" : "wifi_connecting", NULL);
}
static void rtc_task(void *arg) {
    for (;;) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50))) {
            if (peer) {
                int64_t now = esp_timer_get_time();
                if (now - lease > 20000000 || (!connected && now - created_at > 45000000))
                    stop_locked();
                else
                    peer_connection_loop(peer);
            }
            xSemaphoreGive(mutex);
        }
        // Always yield at least one scheduler tick, including IDF's 100 Hz default.
        vTaskDelay(1);
    }
}
void xrtc_peer_init(void) {
    peer_init();
    metrics_init();
    mutex = xSemaphoreCreateMutex();
    sender = calloc(1, sizeof(*sender));
    if (!sender)
        abort();
    hevc_init(sender, esp_random(), esp_random());
    xTaskCreatePinnedToCore(rtc_task, "webrtc_peer", 12288, NULL, 5, NULL, 0);
}
char *rtc_offer(uint32_t *session) {
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(5000)))
        return NULL;
    if (peer) {
        xSemaphoreGive(mutex);
        return NULL;
    }
    PeerConfiguration config = {.video_codec = CODEC_H265,
                                .audio_codec = CODEC_NONE,
                                .datachannel = DATA_CHANNEL_NONE,
                                .on_request_keyframe = request_keyframe};
    peer = peer_connection_create(&config);
    char *offer = NULL;
    if (peer) {
        peer_connection_oniceconnectionstatechange(peer, changed);
        const char *sdp = peer_connection_create_offer(peer);
        if (sdp)
            offer = strdup(sdp);
    }
    if (!offer)
        stop_locked();
    else {
        generation = esp_random();
        if (!generation)
            generation = 1;
        *session = generation;
        metrics_start(generation);
        lease = created_at = esp_timer_get_time();
        hevc_init(sender, esp_random(), esp_random());
    }
    xSemaphoreGive(mutex);
    return offer;
}
esp_err_t rtc_answer(uint32_t session, const char *sdp) {
    if (!sdp || strlen(sdp) > 16000 || !strstr(sdp, "H265/90000") || !strstr(sdp, "a=recvonly") ||
        !strstr(sdp, "a=fingerprint:sha-256 "))
        return ESP_ERR_INVALID_ARG;
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(5000)))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (peer && generation == session) {
        peer_connection_set_remote_description(peer, sdp, SDP_TYPE_ANSWER);
        lease = esp_timer_get_time();
        result = ESP_OK;
    }
    xSemaphoreGive(mutex);
    return result;
}
void rtc_stop(void) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(5000))) {
        stop_locked();
        xSemaphoreGive(mutex);
    }
}
void rtc_stop_session(uint32_t session) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(5000))) {
        if (peer && generation == session) stop_locked();
        xSemaphoreGive(mutex);
    }
}
void rtc_heartbeat(uint32_t session) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50))) {
        if (peer && generation == session)
            lease = esp_timer_get_time();
        xSemaphoreGive(mutex);
    }
}
bool rtc_wanted(void) { return peer != NULL; }
bool rtc_connected(void) { return connected; }
static int send_packet(const uint8_t *packet, size_t bytes, void *ctx) {
    return peer_connection_send_rtp(peer, packet, bytes);
}
void rtc_video(const uint8_t *data, size_t bytes, uint64_t timestamp_ms, int64_t received_us) {
    // Do not block camera input behind the DTLS handshake mutex.
    if (!atomic_load(&connected))
        return;
    // MISS is TCP: bounded backpressure is preferable to silently skipping
    // a reference frame during a brief control-plane scheduling delay.
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(500))) {
        g_status.dropped_frames++;
        atomic_store(&resync_needed, true);
        metrics_frame(FRAME_LOCK_DROP, bytes, timestamp_ms, 0, received_us, esp_timer_get_time());
        return;
    }
    if (peer && connected) {
        if (atomic_exchange(&resync_needed, false))
            sender->waiting_keyframe = true;
        uint64_t before = sender->packets;
        int result = hevc_send(sender, data, bytes, timestamp_ms, send_packet, NULL);
        g_status.rtp_packets += sender->packets - before;
        if (result < 0) {
            g_status.dropped_frames++;
            sender->waiting_keyframe = true;
        }
        uint32_t rtp = sender->initial_timestamp + (uint32_t)((timestamp_ms - sender->base_ms) * 90);
        metrics_frame(result < 0 ? FRAME_SEND_ERROR : result, bytes, timestamp_ms, rtp,
                      received_us, esp_timer_get_time());
    }
    xSemaphoreGive(mutex);
}
