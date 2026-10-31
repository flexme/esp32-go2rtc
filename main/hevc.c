// RFC 7798 HEVC single-NAL and fragmentation-unit packetization.
#include "hevc.h"
#include <string.h>
#define RTP_MAX 1200
#define PAYLOAD_MAX (RTP_MAX - 12)
static void be16(uint8_t *b, uint16_t v) {
    b[0] = v >> 8;
    b[1] = v;
}
static void be32(uint8_t *b, uint32_t v) {
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}
static bool next_nal(const uint8_t *b, size_t n, size_t *pos, const uint8_t **nal, size_t *len) {
    size_t start = n, prefix = 0;
    for (size_t i = *pos; i + 3 <= n; i++) {
        if (!b[i] && !b[i + 1] && b[i + 2] == 1) {
            start = i;
            prefix = 3;
            break;
        }
        if (i + 4 <= n && !b[i] && !b[i + 1] && !b[i + 2] && b[i + 3] == 1) {
            start = i;
            prefix = 4;
            break;
        }
    }
    if (start == n)
        return false;
    size_t end = n;
    for (size_t i = start + prefix; i + 3 <= n; i++) {
        if (!b[i] && !b[i + 1] && (b[i + 2] == 1 || (i + 4 <= n && !b[i + 2] && b[i + 3] == 1))) {
            end = i;
            break;
        }
    }
    *pos = end;
    *nal = b + start + prefix;
    *len = end - start - prefix;
    return true;
}
void hevc_init(hevc_sender_t *s, uint16_t sequence, uint32_t timestamp) {
    memset(s, 0, sizeof(*s));
    s->sequence = sequence;
    s->ssrc = 1;
    s->initial_timestamp = timestamp;
    s->waiting_keyframe = true;
}
static int emit_nal(hevc_sender_t *s, const uint8_t *nal, size_t n, uint32_t ts, bool marker,
                    hevc_send_cb send, void *ctx) {
    if (n < 2 || (nal[0] & 0x80) || !(nal[1] & 7))
        return -1;
    uint8_t packet[RTP_MAX + 32];
    packet[0] = 0x80;
    be32(packet + 4, ts);
    be32(packet + 8, s->ssrc);
    if (n <= PAYLOAD_MAX) {
        packet[1] = 96 | (marker ? 0x80 : 0);
        be16(packet + 2, s->sequence++);
        memcpy(packet + 12, nal, n);
        if (send(packet, n + 12, ctx))
            return -1;
        s->packets++;
        return 0;
    }
    const uint8_t type = (nal[0] >> 1) & 63;
    packet[12] = (nal[0] & 0x81) | (49 << 1);
    packet[13] = nal[1];
    size_t offset = 2;
    while (offset < n) {
        size_t take = n - offset;
        if (take > PAYLOAD_MAX - 3)
            take = PAYLOAD_MAX - 3;
        bool last = offset + take == n;
        packet[1] = 96 | (last && marker ? 0x80 : 0);
        be16(packet + 2, s->sequence++);
        packet[14] = type | (offset == 2 ? 0x80 : 0) | (last ? 0x40 : 0);
        memcpy(packet + 15, nal + offset, take);
        if (send(packet, take + 15, ctx))
            return -1;
        s->packets++;
        offset += take;
    }
    return 0;
}
int hevc_send(hevc_sender_t *s, const uint8_t *b, size_t n, uint64_t ms, hevc_send_cb send,
              void *ctx) {
    if (!b || n < 5 || n > 512 * 1024)
        return -1;
    size_t pos = 0, len, last_payload_end = 0;
    const uint8_t *nal;
    bool key = false, vcl = false;
    while (next_nal(b, n, &pos, &nal, &len)) {
        if (len < 2)
            continue;
        int type = (nal[0] >> 1) & 63;
        if (type >= 32 && type <= 34) {
            if (len > sizeof(s->parameter_sets[0]))
                return -1;
            memcpy(s->parameter_sets[type - 32], nal, len);
            s->parameter_sizes[type - 32] = len;
        }
        if (type < 32)
            vcl = true;
        if (type < 32 || type > 34)
            last_payload_end = pos;
        if (type >= 16 && type <= 21)
            key = true;
    }
    if (!vcl)
        return 0;
    if (s->waiting_keyframe &&
        (!key || !s->parameter_sizes[0] || !s->parameter_sizes[1] || !s->parameter_sizes[2]))
        return 0;
    if (!s->started) {
        s->base_ms = ms;
        s->started = true;
    }
    if (ms < s->base_ms) {
        s->base_ms = ms;
        s->waiting_keyframe = true;
        return 0;
    }
    uint32_t ts = s->initial_timestamp + (uint32_t)((ms - s->base_ms) * 90);
    if (key) {
        for (int i = 0; i < 3; i++)
            if (s->parameter_sizes[i] &&
                emit_nal(s, s->parameter_sets[i], s->parameter_sizes[i], ts, false, send, ctx))
                return -1;
        s->waiting_keyframe = false;
    }
    pos = 0;
    while (next_nal(b, n, &pos, &nal, &len)) {
        if (len < 2)
            continue;
        int type = (nal[0] >> 1) & 63;
        if (type >= 32 && type <= 34)
            continue;
        if (emit_nal(s, nal, len, ts, pos == last_payload_end, send, ctx)) {
            s->waiting_keyframe = true;
            return -1;
        }
    }
    return 1;
}
