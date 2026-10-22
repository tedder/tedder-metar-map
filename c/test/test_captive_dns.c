/* Host unit test for main/captive_dns.c.  Run: make -C test */
#include <stdio.h>
#include <string.h>

#include "../main/captive_dns.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static const uint32_t IP = 0x0104A8C0;   /* 192.168.4.1 in network byte order (little-endian host) */

/* Build a query: id 0x1234, RD set, one question for `name` with `qtype`; optional EDNS OPT RR */
static int make_query(uint8_t *b, const char *name, int qtype, int edns)
{
    uint8_t h[12] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, edns ? 1 : 0 };
    memcpy(b, h, 12);
    int p = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        int L = dot ? (int)(dot - s) : (int)strlen(s);
        b[p++] = (uint8_t)L;
        memcpy(&b[p], s, L);
        p += L;
        s += L + (dot ? 1 : 0);
    }
    b[p++] = 0;
    b[p++] = 0; b[p++] = (uint8_t)qtype;   /* QTYPE */
    b[p++] = 0; b[p++] = 1;                /* QCLASS IN */
    if (edns) {                            /* OPT: root, type 41, udp 1232, ttl 0, rdlen 0 */
        uint8_t opt[11] = { 0, 0, 41, 0x04, 0xD0, 0, 0, 0, 0, 0, 0 };
        memcpy(&b[p], opt, sizeof(opt));
        p += sizeof(opt);
    }
    return p;
}

int main(void)
{
    uint8_t b[512];

    /* A query (iOS: captive.apple.com) → one answer with the AP IP */
    int q = make_query(b, "captive.apple.com", 1, 0);
    int r = captive_dns_reply(b, q, sizeof(b), IP);
    CHECK(r == q + 16);
    CHECK(b[0] == 0x12 && b[1] == 0x34);                 /* id kept */
    CHECK(b[2] == 0x85);                                 /* QR + AA + RD */
    CHECK(b[3] == 0x80);                                 /* RA, NOERROR */
    CHECK(b[7] == 1 && b[11] == 0);                      /* ANCOUNT 1, ARCOUNT 0 */
    CHECK(b[q] == 0xC0 && b[q + 1] == 0x0C);             /* name pointer to the question */
    CHECK(b[q + 3] == 1 && b[q + 9] == 60 && b[q + 11] == 4);  /* type A, TTL 60, rdlen 4 */
    CHECK(memcmp(&b[q + 12], &IP, 4) == 0);

    /* Android: connectivitycheck.gstatic.com with EDNS OPT → OPT dropped, still answered */
    q = make_query(b, "connectivitycheck.gstatic.com", 1, 1);
    int qnoopt = q - 11;
    r = captive_dns_reply(b, q, sizeof(b), IP);
    CHECK(r == qnoopt + 16);
    CHECK(b[7] == 1 && b[11] == 0);

    /* AAAA → NOERROR, no answer (phones then use the A answer) */
    q = make_query(b, "captive.apple.com", 28, 0);
    r = captive_dns_reply(b, q, sizeof(b), IP);
    CHECK(r == q);
    CHECK(b[2] == 0x85 && b[3] == 0x80 && b[7] == 0);

    /* rejected packets */
    CHECK(captive_dns_reply(b, 11, sizeof(b), IP) == -1);               /* short */
    q = make_query(b, "a.b", 1, 0); b[2] |= 0x80;                       /* a response, not a query */
    CHECK(captive_dns_reply(b, q, sizeof(b), IP) == -1);
    q = make_query(b, "a.b", 1, 0); b[5] = 2;                           /* two questions */
    CHECK(captive_dns_reply(b, q, sizeof(b), IP) == -1);
    q = make_query(b, "a.b", 1, 0); b[2] = 0x28;                        /* opcode UPDATE */
    CHECK(captive_dns_reply(b, q, sizeof(b), IP) == -1);
    q = make_query(b, "example.com", 1, 0);
    CHECK(captive_dns_reply(b, q - 3, sizeof(b), IP) == -1);            /* truncated question */
    q = make_query(b, "example.com", 1, 0); b[12] = 0xC0;               /* compression pointer */
    CHECK(captive_dns_reply(b, q, sizeof(b), IP) == -1);
    q = make_query(b, "example.com", 1, 0);
    CHECK(captive_dns_reply(b, q, (size_t)q + 15, IP) == -1);           /* no room for answer */
    memset(b, 0, sizeof(b)); b[5] = 1;                                  /* label runs off the end */
    b[12] = 63;
    CHECK(captive_dns_reply(b, 20, sizeof(b), IP) == -1);

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
