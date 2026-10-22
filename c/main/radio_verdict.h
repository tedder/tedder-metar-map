/* Antenna / radio check verdict (POST /radio_check). Pure C, host-tested (test/test_radio_verdict.c).
 *
 * The check stays on the current network and steps the TX power (17, 11, 8 dBm, twice). Bad antennas
 * on one bench board (2026-10) showed one signature: the link gets worse as power goes UP
 * (upload 55 kB/s at 17 dBm vs 485 at 11; never joined at 20). Units run at 11 by default and never
 * above 17 (0.4.13), so the board is judged at 11: trouble only at 17 is a warning, not a failure. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define RC_LEVELS 3        /* 17, 11, 8 dBm */
#define RC_ROUNDS 2

typedef struct {
    int dbm;
    int sent, recv;        /* gateway pings */
    int avg_ms, max_ms;
    int up_kBps;           /* -1 = not measured (no sink), 0 = upload failed */
    int dropped;           /* WiFi disconnect reason during this step, 0 = none */
} rc_row_t;

/* RC_WARN: fine at 11 and 8 (ships), weak at 17 (leave wifi_tx_power_dbm at the default) */
typedef enum { RC_PASS, RC_FAIL, RC_UNSURE, RC_WARN } rc_result_t;

/** Judge rows (any order, any count; each dbm should be one of 17/11/8). `why` gets one plain
 *  sentence. */
rc_result_t rc_verdict(const rc_row_t *rows, int n, char *why, size_t why_sz);

const char *rc_result_name(rc_result_t r);

/** True for "http://a.b.c.d" or "http://a.b.c.d:port" with a private IPv4 (10/8, 172.16/12,
 *  192.168/16): the check uploads only to a sink on the LAN. */
bool rc_sink_ok(const char *url);
