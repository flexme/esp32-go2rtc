#ifndef PEER_H_
#define PEER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "peer_connection.h"
#include "peer_signaling.h"

int peer_init();
// Cumulative UDP retry/failure counters, including STUN/DTLS/media.
void peer_udp_stats(uint32_t* retries, uint32_t* failures);
void peer_repair_stats(uint32_t* feedback, uint32_t* resent, uint32_t* missed);

void peer_deinit();

#ifdef __cplusplus
}
#endif

#endif  // PEER_H_

