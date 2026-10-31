// Bounded metadata only; never records camera payloads or account credentials.
#include "metrics.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "peer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TIMING_CAPACITY 128
typedef struct {
    uint32_t rtp, bytes;
    int64_t received_us, sent_us;
} frame_timing_t;
typedef struct {
    uint32_t session, max_frame_bytes, pli_requests;
    uint64_t received, sent, gated, lock_drops, send_errors, relay_total_us, relay_max_us;
    uint64_t source_ms;
    int64_t last_received_us, started_us;
} media_metrics_t;
static SemaphoreHandle_t lock;
static frame_timing_t *ring;
static size_t used, next;
static media_metrics_t media;

void metrics_init(void) {
    lock = xSemaphoreCreateMutex();
    ring = heap_caps_calloc(TIMING_CAPACITY, sizeof(*ring), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lock || !ring)
        abort();
}
void metrics_start(uint32_t session) {
    xSemaphoreTake(lock, portMAX_DELAY);
    memset(&media, 0, sizeof(media));
    media.session = session;
    media.started_us = esp_timer_get_time();
    used = next = 0;
    xSemaphoreGive(lock);
}
void metrics_pli(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    media.pli_requests++;
    xSemaphoreGive(lock);
}
void metrics_frame(int result, size_t bytes, uint64_t source_ms, uint32_t rtp,
                   int64_t received_us, int64_t sent_us) {
    xSemaphoreTake(lock, portMAX_DELAY);
    media.received++;
    media.source_ms = source_ms;
    media.last_received_us = received_us;
    if (bytes > media.max_frame_bytes)
        media.max_frame_bytes = bytes;
    if (result == FRAME_SENT) {
        media.sent++;
        uint64_t relay = sent_us > received_us ? sent_us - received_us : 0;
        media.relay_total_us += relay;
        if (relay > media.relay_max_us)
            media.relay_max_us = relay;
        ring[next] = (frame_timing_t){rtp, bytes, received_us, sent_us};
        next = (next + 1) % TIMING_CAPACITY;
        if (used < TIMING_CAPACITY)
            used++;
    } else if (result == FRAME_LOCK_DROP)
        media.lock_drops++;
    else if (result == FRAME_SEND_ERROR)
        media.send_errors++;
    else
        media.gated++;
    xSemaphoreGive(lock);
}
cJSON *metrics_status(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    media_metrics_t m = media;
    xSemaphoreGive(lock);
    cJSON *o = cJSON_CreateObject();
#define ADD(name, value) cJSON_AddNumberToObject(o, name, (double)(value))
    ADD("session", m.session);
    ADD("received", m.received);
    ADD("sent", m.sent);
    ADD("gated", m.gated);
    ADD("lock_drops", m.lock_drops);
    ADD("send_errors", m.send_errors);
    ADD("pli_requests", m.pli_requests);
    ADD("max_frame_bytes", m.max_frame_bytes);
    ADD("relay_average_ms", m.sent ? (double)m.relay_total_us / m.sent / 1000 : 0);
    ADD("relay_max_ms", m.relay_max_us / 1000.0);
    ADD("source_timestamp_ms", m.source_ms);
    ADD("last_received_mono_ms", m.last_received_us / 1000.0);
    ADD("started_mono_ms", m.started_us / 1000.0);
    uint32_t retries, failures;
    peer_udp_stats(&retries, &failures);
    ADD("udp_retries_since_boot", retries);
    ADD("udp_failures_since_boot", failures);
    uint32_t feedback, resent, missed;
    peer_repair_stats(&feedback, &resent, &missed);
    ADD("nack_feedback_since_boot", feedback);
    ADD("retransmitted_packets_since_boot", resent);
    ADD("retransmit_cache_misses_since_boot", missed);
#undef ADD
    return o;
}
cJSON *metrics_timings(void) {
    frame_timing_t *copy = malloc(TIMING_CAPACITY * sizeof(*copy));
    if (!copy)
        return NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t count = used, first = (next + TIMING_CAPACITY - used) % TIMING_CAPACITY;
    uint32_t session = media.session;
    for (size_t i = 0; i < count; i++)
        copy[i] = ring[(first + i) % TIMING_CAPACITY];
    xSemaphoreGive(lock);
    // Format a bounded numeric array directly: hundreds of tiny cJSON nodes
    // would otherwise consume scarce internal Wi-Fi/TLS heap during polling.
    size_t capacity = count * 112 + 128, offset = 0;
    char *text = malloc(capacity);
    if (!text) {
        free(copy);
        return NULL;
    }
    offset += snprintf(text + offset, capacity - offset, "{\"session\":%lu,\"frames\":[",
                       (unsigned long)session);
    for (size_t i = 0; i < count; i++) {
        offset += snprintf(text + offset, capacity - offset, "%s[%lu,%lld,%lld,%lu]",
                           i ? "," : "", (unsigned long)copy[i].rtp,
                           (long long)copy[i].received_us, (long long)copy[i].sent_us,
                           (unsigned long)copy[i].bytes);
    }
    snprintf(text + offset, capacity - offset, "]}");
    cJSON *o = cJSON_CreateRaw(text);
    free(text);
    free(copy);
    return o;
}
