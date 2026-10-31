// MIT. Bounded same-SSRC retransmission of unchanged, already protected RTP.
#pragma once
#include <stddef.h>
#include <stdint.h>
#define REPAIR_PACKETS 512
#define REPAIR_PACKET_BYTES 1428
typedef struct {
    uint16_t sequence, size;
    uint8_t packet[REPAIR_PACKET_BYTES];
} RepairPacket;
typedef struct {
    RepairPacket packets[REPAIR_PACKETS];
    uint32_t feedback, resent, missed;
} RtpRepair;
typedef int (*repair_send_cb)(const uint8_t *, size_t, void *);
void rtp_repair_store(RtpRepair *, const uint8_t *, size_t);
void rtp_repair_nack(RtpRepair *, const uint8_t *, size_t, uint32_t media_ssrc,
                     repair_send_cb, void *, unsigned budget);
