#include "rtp_repair.h"
#include <string.h>
static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)be16(p) << 16) | be16(p + 2); }
void rtp_repair_store(RtpRepair *r, const uint8_t *data, size_t size) {
    if (!r || size < 12 || size > REPAIR_PACKET_BYTES)
        return;
    uint16_t seq = be16(data + 2);
    RepairPacket *p = &r->packets[seq % REPAIR_PACKETS];
    p->sequence = seq;
    p->size = size;
    memcpy(p->packet, data, size);
}
void rtp_repair_nack(RtpRepair *r, const uint8_t *data, size_t size, uint32_t media_ssrc,
                     repair_send_cb send, void *ctx, unsigned budget) {
    // Called only after successful SRTCP authentication/decryption.
    if (!r || !send || size < 16 || (data[0] >> 6) != 2 || (data[0] & 31) != 1 ||
        data[1] != 205 || ((size_t)be16(data + 2) + 1) * 4 != size ||
        be32(data + 8) != media_ssrc)
        return;
    if (data[0] & 32) {
        unsigned padding = data[size - 1];
        if (!padding || padding > size - 12)
            return;
        size -= padding;
    }
    if ((size - 12) % 4)
        return;
    r->feedback++;
    if (budget > 32)
        budget = 32;
    for (size_t pos = 12; pos + 4 <= size && budget; pos += 4) {
        uint16_t pid = be16(data + pos), mask = be16(data + pos + 2);
        for (unsigned bit = 0; bit <= 16 && budget; bit++) {
            if (bit && !(mask & (1u << (bit - 1))))
                continue;
            budget--;
            uint16_t seq = (uint16_t)(pid + bit);
            RepairPacket *p = &r->packets[seq % REPAIR_PACKETS];
            if (p->size < 12 || p->sequence != seq) {
                r->missed++;
                continue;
            }
            // Never encrypt a repeated sequence again. Preserve the original
            // ciphertext/authentication tag and receiver replay semantics.
            if (send(p->packet, p->size, ctx))
                return;
            r->resent++;
        }
    }
}
