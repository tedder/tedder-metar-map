#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * WiFi TX power autotune. Pure state machine (no ESP-IDF calls) so it can be unit-tested on the
 * host (test/test_tx_tune.c); main.c applies the power and saves NVS when told to.
 *
 * Down: one bench board hears the AP fine but gets no answer to its auth frames at 19-20 dBm
 * (reason 2 AUTH_EXPIRE). After `fails` of those at RSSI >= min_rssi since the last IP, the cap
 * drops `step` dB (not below `floor`). The level that gets an IP is saved (NVS tx_auto).
 *
 * Up: while below the cap and connected, every `up_every_ms` try 1 dB more: leave the network and
 * rejoin at the new level (auth is where this board fails), then watch the weather checks for
 * `up_watch_ms`. Any disconnect while rejoining or watching, or `up_fail_run` weather checks in a
 * row that could not reach the server, goes back down and waits `up_retry_fail_ms`. Passing needs
 * the watch time and `up_ok_min` checks that reached the server. No IP within `up_join_ms` of
 * starting also fails. Any HTTP answer counts as
 * reaching it (a 404/500/503 proves the radio link works); only transport failures count against.
 */

typedef struct {
    int     fails, step, floor, min_rssi;
    int64_t up_every_ms, up_watch_ms, up_retry_fail_ms, up_join_ms;
    int     up_ok_min, up_fail_run;
} tx_tune_params_t;

extern const tx_tune_params_t TX_TUNE_DEFAULTS;

typedef enum { TXP_IDLE, TXP_LEAVING, TXP_JOINING, TXP_WATCH } tx_probe_state_t;

typedef struct {
    tx_tune_params_t p;
    int              cap;         /* wifi_tx_power_dbm */
    bool             fixed;       /* cap set explicitly: no autotune */
    int              cur;         /* level to apply */
    int              learned;     /* saved level, 0 = none */
    int              auth_exp;    /* strong-signal auth timeouts since the last IP */
    int64_t          next_up_ms;  /* earliest step-up test; 0 = none planned */
    tx_probe_state_t probe;
    int              prev;        /* level to go back to */
    int64_t          probe_at;    /* when the step-up test started */
    int64_t          watch_from;
    int              ok, fail_run;
    /* since the last IP, for the LEDs: AP heard but no auth answer; password rejected */
    bool             no_answer, bad_pass;
    /* counters for the status page / tests */
    int              downs, ups, up_fails;
} tx_tune_t;

/* Actions for the caller (bitmask). */
enum {
    TXT_APPLY     = 1,    /* set the radio to t->cur */
    TXT_SAVE      = 2,    /* NVS tx_auto = t->learned */
    TXT_ERASE     = 4,    /* NVS tx_auto erase (back at the cap) */
    TXT_RECONNECT = 8,    /* leave and rejoin (step-up test) */
    TXT_LOWERED   = 16,   /* retry the same network at the new level (don't rotate slots) */
    TXT_OURS      = 32,   /* this disconnect was our own leave: not a drop */
};

void tx_tune_init(tx_tune_t *t, const tx_tune_params_t *p, int cap, bool fixed, int saved, int64_t now);
int  tx_tune_disconnect(tx_tune_t *t, int reason, int rssi, int64_t now);
int  tx_tune_got_ip(tx_tune_t *t, int64_t now);
int  tx_tune_fetch(tx_tune_t *t, bool reached_server, int64_t now);
int  tx_tune_tick(tx_tune_t *t, bool connected, int64_t now);

/* What the LEDs should say while not connected (priority: password, then no answer). */
typedef enum { TXL_NONE, TXL_NO_ANSWER, TXL_NO_ANSWER_FLOOR, TXL_BAD_PASS } tx_led_t;
tx_led_t tx_tune_led(const tx_tune_t *t);
