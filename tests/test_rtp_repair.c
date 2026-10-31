#include "rtp_repair.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static unsigned sent;
static uint16_t seqs[32];
static int collect(const uint8_t *data, size_t size, void *ctx) {
    assert(size == 20 && data[19] == 0xa5); // Opaque original authentication tag preserved.
    seqs[sent++] = ((uint16_t)data[2] << 8) | data[3];
    return 0;
}
static void store(RtpRepair *r, uint16_t seq) {
    uint8_t packet[20] = {0x80, 96};
    packet[2] = seq >> 8; packet[3] = seq; packet[19] = 0xa5;
    rtp_repair_store(r, packet, sizeof(packet));
}
int main(void) {
    RtpRepair *r = calloc(1,sizeof(*r)); assert(r);
    store(r,65535); store(r,0); store(r,2);
    uint8_t nack[16] = {0x81,205,0,3,0,0,0,9,0,0,0,1,0xff,0xff,0,5};
    rtp_repair_nack(r,nack,16,1,collect,NULL,16);
    assert(sent==3 && seqs[0]==65535 && seqs[1]==0 && seqs[2]==2);
    assert(r->feedback==1 && r->resent==3 && r->missed==0);
    // Cache collision must not replay an unrelated newer sequence.
    store(r,511); sent=0;
    rtp_repair_nack(r,nack,16,1,collect,NULL,1);
    assert(sent==0 && r->missed==1);
    // Wrong SSRC, truncation, malformed length and partial FCI produce no output.
    rtp_repair_nack(r,nack,16,2,collect,NULL,16);
    for(size_t n=0;n<16;n++)rtp_repair_nack(r,nack,n,1,collect,NULL,16);
    nack[3]=4;rtp_repair_nack(r,nack,16,1,collect,NULL,16);
    assert(sent==0);
    // A feedback budget bounds even an all-bits-set loss report.
    nack[3]=3;nack[12]=0;nack[13]=0;nack[14]=0xff;nack[15]=0xff;
    for(unsigned i=0;i<17;i++)store(r,i);
    rtp_repair_nack(r,nack,16,1,collect,NULL,2);
    assert(sent==2);
    free(r);puts("RTCP NACK wrap, unchanged protected packets, bounds and budgets: PASS");
}
