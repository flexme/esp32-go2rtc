#pragma once
#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

enum { FRAME_GATED = 0, FRAME_SENT = 1, FRAME_LOCK_DROP = -1, FRAME_SEND_ERROR = -2 };
void metrics_init(void);
void metrics_start(uint32_t session);
void metrics_pli(void);
void metrics_frame(int result, size_t bytes, uint64_t source_ms, uint32_t rtp,
                   int64_t received_us, int64_t sent_us);
cJSON *metrics_status(void);
cJSON *metrics_timings(void);
