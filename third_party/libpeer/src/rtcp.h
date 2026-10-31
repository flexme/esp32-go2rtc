#ifndef RTCP_H_
#define RTCP_H_

#include <stddef.h>
#include <stdint.h>

typedef enum RtcpType {

  RTCP_FIR = 192,
  RTCP_SR = 200,
  RTCP_RR = 201,
  RTCP_SDES = 202,
  RTCP_BYE = 203,
  RTCP_APP = 204,
  RTCP_RTPFB = 205,
  RTCP_PSFB = 206,
  RTCP_XR = 207,

} RtcpType;

typedef struct RtcpHeader {
  uint8_t vprc; /* Version, Padding, Report count/Feedback message type */
  uint8_t type; /* Packet Type */
  uint16_t length;

} RtcpHeader;

static inline void rtcp_header_init(RtcpHeader* header, uint8_t type, uint8_t rc) {
  header->vprc = 0x80U | (rc & 0x1fU);
  header->type = type;
}

static inline uint8_t rtcp_header_version(const RtcpHeader* header) {
  return (header->vprc >> 6) & 0x03U;
}

static inline uint8_t rtcp_header_padding(const RtcpHeader* header) {
  return (header->vprc >> 5) & 0x01U;
}

static inline uint8_t rtcp_header_rc(const RtcpHeader* header) {
  return header->vprc & 0x1fU;
}

typedef struct RtcpReportBlock {
  uint32_t ssrc;
  uint32_t flcnpl;
  uint32_t ehsnr;
  uint32_t jitter;
  uint32_t lsr;
  uint32_t dlsr;

} RtcpReportBlock;

typedef struct RtcpRr {
  RtcpHeader header;
  uint32_t ssrc;
  RtcpReportBlock report_block[1];

} RtcpRr;

typedef struct RtcpFir {
  uint32_t ssrc;
  uint32_t seqnr;

} RtcpFir;

typedef struct RtcpFb {
  RtcpHeader header;
  uint32_t ssrc;
  uint32_t media;
  char fci[1];

} RtcpFb;

int rtcp_probe(uint8_t* packet, size_t size);

int rtcp_get_pli(uint8_t* packet, int len, uint32_t ssrc);

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr);

RtcpRr rtcp_parse_rr(uint8_t* packet);

#endif  // RTCP_H_

