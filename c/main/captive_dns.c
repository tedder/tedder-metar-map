#include "captive_dns.h"

#include <string.h>

int captive_dns_reply(uint8_t *buf, int n, size_t cap, uint32_t ip_be)
{
    /* header 12 bytes; QR=0, opcode 0 (standard query); exactly one question */
    if (n < 12 || (buf[2] & 0xF8) != 0 || buf[4] != 0 || buf[5] != 1)
        return -1;
    int p = 12;
    while (p < n && buf[p]) {
        if ((buf[p] & 0xC0) != 0)                 /* compression pointer not valid in a query name */
            return -1;
        p += buf[p] + 1;
    }
    p += 1 + 4;                                   /* root label + QTYPE + QCLASS */
    if (p > n)
        return -1;
    int is_a = buf[p - 4] == 0 && buf[p - 3] == 1;
    if ((size_t)p + (is_a ? 16 : 0) > cap)
        return -1;

    buf[2] = 0x84 | (buf[2] & 0x01);              /* QR + AA, keep RD */
    buf[3] = 0x80;                                /* RA, NOERROR */
    buf[6] = 0; buf[7] = is_a ? 1 : 0;            /* ANCOUNT */
    memset(&buf[8], 0, 4);                        /* drop NS/AR (EDNS OPT etc.) */
    int len = p;
    if (is_a) {
        static const uint8_t ans[12] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
        memcpy(&buf[len], ans, sizeof(ans));
        memcpy(&buf[len + 12], &ip_be, 4);
        len += 16;
    }
    return len;
}
