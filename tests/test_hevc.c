#include "hevc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t recovered[16384];
static size_t recovered_size, packets, markers;
static uint16_t sequence;
static uint32_t timestamp;
static int collect(const uint8_t *b, size_t n, void *ctx) {
    assert(n >= 14 && n <= 1200 && b[0] == 0x80 && (b[1] & 127) == 96);
    uint16_t seq = ((uint16_t)b[2] << 8) | b[3];
    if (packets)
        assert(seq == (uint16_t)(sequence + 1));
    sequence = seq;
    uint32_t ts = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    if (packets)
        assert(ts == timestamp);
    timestamp = ts;
    packets++;
    markers += (b[1] >> 7);
    const uint8_t *p = b + 12;
    size_t length = n - 12;
    int type = (p[0] >> 1) & 63;
    if (type == 49) {
        assert(length >= 4);
        if (p[2] & 0x80) {
            assert(recovered_size == 0);
            recovered[recovered_size++] = (p[0] & 0x81) | ((p[2] & 63) << 1);
            recovered[recovered_size++] = p[1];
        }
        assert(recovered_size + length - 3 < sizeof(recovered));
        memcpy(recovered + recovered_size, p + 3, length - 3);
        recovered_size += length - 3;
    } else if (type < 32) {
        assert(recovered_size == 0);
        memcpy(recovered, p, length);
        recovered_size = length;
    }
    return 0;
}
static size_t append(uint8_t *b, size_t at, int type, size_t size) {
    b[at++] = 0;
    b[at++] = 0;
    b[at++] = 0;
    b[at++] = 1;
    b[at++] = type << 1;
    b[at++] = 1;
    memset(b + at, 0x55, size - 2);
    return at + size - 2;
}
int main(void) {
    hevc_sender_t *s = calloc(1, sizeof(*s));
    uint8_t b[10000];
    size_t n = 0;
    hevc_init(s, 65534, 100);
    n = append(b, 0, 1, 50);
    assert(hevc_send(s, b, n, 1000, collect, NULL) == 0 && packets == 0);
    n = 0;
    n = append(b, n, 32, 10);
    n = append(b, n, 33, 12);
    n = append(b, n, 34, 8);
    n = append(b, n, 19, 5000);
    assert(hevc_send(s, b, n, 1000, collect, NULL) == 1);
    assert(packets == 8 && markers == 1 && recovered_size == 5000 && timestamp == 100);
    assert(recovered[0] == (19 << 1) && recovered[1] == 1);
    for (size_t i = 2; i < 5000; i++)
        assert(recovered[i] == 0x55);
    packets = markers = recovered_size = 0;
    n = append(b, 0, 1, 120);
    assert(hevc_send(s, b, n, 1060, collect, NULL) == 1);
    assert(packets == 1 && markers == 1 && timestamp == 5500);
    packets = markers = recovered_size = 0;
    s->waiting_keyframe = true;
    assert(hevc_send(s, b, n, 1120, collect, NULL) == 0 && packets == 0);
    // Truncation and malformed inputs must remain bounded under ASan/UBSan.
    hevc_init(s, 0, 0);
    for (size_t i = 0; i < 2000; i++) {
        for (size_t j = 0; j < i; j++)
            b[j] = (uint8_t)(j * 17 + i);
        hevc_send(s, b, i, 1, collect, NULL);
    }
    free(s);
    puts("HEVC RFC7798 round-trip, timestamp, wraparound, gating and bounds: PASS");
    return 0;
}
