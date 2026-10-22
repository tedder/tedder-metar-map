#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * Turn a DNS query in `buf` (length `n`, buffer capacity `cap`) into the captive-portal reply,
 * in place: every A query is answered with `ip_be` (network byte order, TTL 60); other query
 * types get NOERROR with no answer. Returns the reply length, or -1 if the packet is not a
 * standard single-question query (caller should drop it).
 * Pure function so it can be unit-tested on the host (test/test_captive_dns.c).
 */
int captive_dns_reply(uint8_t *buf, int n, size_t cap, uint32_t ip_be);
