#include "kicksmash_sim.h"
#include <stdio.h>
#include <string.h>

#define ROM_BASE 0x00f80000u
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static const uint16_t magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };
static unsigned reqpos, reqneed, reqshift=2, replen, reppos;
static uint16_t reqlen, reqcmd;
static uint8_t reply[512];

static uint32_t crc32be(uint32_t crc, const uint8_t *p, unsigned n) {
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (unsigned b = 0; b < 8; b++)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
    return crc;
}
static void put16(uint8_t *p, unsigned *n, uint16_t v) {
    p[(*n)++] = v >> 8; p[(*n)++] = v;
}
static void put32(uint8_t *p, unsigned *n, uint32_t v) {
    put16(p, n, v >> 16); put16(p, n, v);
}
static unsigned id_data(uint8_t *p) {
    unsigned n = 0;
    put16(p,&n,1); put16(p,&n,5);
    p[n++]=20; p[n++]=26; p[n++]=9; p[n++]=7;
    p[n++]=12; p[n++]=0; p[n++]=0; p[n++]=0;
    memset(p+n,0,24); memcpy(p+n,"OMEGA-SIM",9); n+=24;
    put16(p,&n,1); put16(p,&n,1); put32(p,&n,0x12091610);
    memset(p+n,0,16); memcpy(p+n,"Omega emulator",14); n+=16;
    p[n++]=0; p[n++]=0; put16(p,&n,0); memset(p+n,0,24); n+=24;
    return n;
}
static unsigned bank_data(uint8_t *p) {
    static const char *name[8] = { "Kickstart 1.3", "Kickstart 2.04",
        "Kickstart 3.1.4", "DiagROM", "ROM switcher", "", "", "" };
    unsigned n=0; p[n++]=1; p[n++]=4; p[n++]=1; p[n++]=2;
    memset(p+n,0xff,8); p[n]=4; p[n+1]=2; n+=8;
    memset(p+n,0,8); n+=8;
    for (unsigned i=0;i<8;i++) { memset(p+n,0,16); strncpy((char*)p+n,name[i],15); n+=16; }
    memset(p+n,0,12); return n+12;
}
static void make_reply(void) {
    uint8_t data[256]; unsigned dlen=0, n=0; uint16_t status=0;
    switch (reqcmd) {
    case 0x0001: break;
    case 0x0002: dlen=id_data(data); break;
    case 0x0020: dlen=bank_data(data); break;
    case 0x0208: data[0]=0; data[1]=4; dlen=2; break;
    default: status=0x0300; break;
    }
    /* ks_reply() presents two consecutive protocol words per 32-bit read. */
    for(unsigned i=0;i<COUNT(magic);i++) put16(reply,&n,magic[i]);
    put16(reply,&n,dlen); put16(reply,&n,status);
    memcpy(reply+n,data,dlen); n+=dlen;
    while(n&3) reply[n++]=0;
    uint8_t crc_header[4] = { dlen >> 8, dlen, status >> 8, status };
    uint32_t crc=crc32be(0,crc_header,4); crc=crc32be(crc,data,dlen);
    put32(reply,&n,crc);
    replen=n; reppos=0;
    printf("[KickSmash] command 0x%04x -> 0x%04x (%u bytes)\n",reqcmd,status,dlen);
}
int kicksmash_sim_read_long(uint32_t a, uint32_t *v) {
    /* While its reply DMA is active, the real board drives every ROM read;
     * transport revisions use different harmless addresses to clock it. */
    if(reppos<replen && a>=ROM_BASE && a<ROM_BASE+0x80000) {
        uint8_t *p=reply+reppos; *v=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        reppos+=4; return 1;
    }
    if(a==ROM_BASE+0x1554 && reqneed && reqpos>=reqneed) {
        make_reply(); reqpos=reqneed=0;
        /* This switcher transport uses the trigger read as reply long 0. */
        uint8_t *p=reply;
        *v=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
        reppos=4;
        return 1;
    }
    if(a<ROM_BASE || a>=ROM_BASE+0x40000 || (a&1)) return 0;
    uint32_t off=a-ROM_BASE;
    if (reqpos == 0) {
        if (off == ((uint32_t)magic[0] << 1)) reqshift=1;
        else if (off == ((uint32_t)magic[0] << 2)) reqshift=2;
        else return 0;
        printf("[KickSmash] request preamble detected (%u-bit ROM mode)\n",
               reqshift == 1 ? 16 : 32);
    }
    if (off & ((1u << reqshift) - 1)) return 0;
    uint16_t w=off>>reqshift;
    if(reqpos<COUNT(magic)) {
        if(w==magic[reqpos]) reqpos++; else reqpos=(w==magic[0]);
        return 0;
    }
    reqpos++;
    if(reqpos==5) reqlen=w;
    if(reqpos==6) { reqcmd=w; unsigned nw=(reqlen+1)/2; reqneed=6+nw+(nw&1)+2; }
    if(reqpos>260) reqpos=reqneed=0;
    return 0;
}
