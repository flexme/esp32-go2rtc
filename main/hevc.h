#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int (*hevc_send_cb)(const uint8_t *, size_t, void *);
typedef struct {
    uint16_t sequence;
    uint32_t ssrc, initial_timestamp;
    uint64_t base_ms;
    bool started, waiting_keyframe;
    uint8_t parameter_sets[3][4096];
    size_t parameter_sizes[3];
    uint64_t packets;
} hevc_sender_t;
void hevc_init(hevc_sender_t *s, uint16_t sequence, uint32_t timestamp);
int hevc_send(hevc_sender_t *s, const uint8_t *annexb, size_t size, uint64_t timestamp_ms,
              hevc_send_cb send, void *ctx);
