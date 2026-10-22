#include "tx_tune.h"

#include <string.h>

#define REASON_AUTH_EXPIRE 2    /* WIFI_REASON_AUTH_EXPIRE */
#define REASON_ASSOC_LEAVE 8    /* WIFI_REASON_ASSOC_LEAVE: our own esp_wifi_disconnect() */
/* wrong password: WPA2 4-way handshake timeout (15, 204 in IDF), SAE auth failure (202) */
#define REASON_4WAY_TIMEOUT     15
#define REASON_AUTH_FAIL        202
#define REASON_HANDSHAKE_TIMEOUT 204

#define DAY_MS (24LL * 3600 * 1000)

const tx_tune_params_t TX_TUNE_DEFAULTS = {
    .fails = 2, .step = 3, .floor = 8, .min_rssi = -65,
    .up_every_ms = 7 * DAY_MS, .up_watch_ms = 10 * 60 * 1000, .up_retry_fail_ms = 30 * DAY_MS,
    .up_join_ms = 60 * 1000,
    .up_ok_min = 3, .up_fail_run = 2,
};

static void plan_up(tx_tune_t *t, int64_t now, int64_t wait)
{
    t->next_up_ms = (!t->fixed && t->cur < t->cap) ? now + wait : 0;
}

void tx_tune_init(tx_tune_t *t, const tx_tune_params_t *p, int cap, bool fixed, int saved, int64_t now)
{
    memset(t, 0, sizeof(*t));
    t->p       = *p;
    t->cap     = cap;
    t->fixed   = fixed;
    t->learned = (!fixed && saved >= p->floor && saved < cap) ? saved : 0;
    t->cur     = t->learned ? t->learned : cap;
    plan_up(t, now, p->up_every_ms);
}

/** Step-up test failed: back to the old level, wait longer before the next try. */
static int up_fail(tx_tune_t *t, int64_t now)
{
    t->cur   = t->prev;
    t->probe = TXP_IDLE;
    t->up_fails++;
    plan_up(t, now, t->p.up_retry_fail_ms);
    return TXT_APPLY | TXT_LOWERED;
}

int tx_tune_disconnect(tx_tune_t *t, int reason, int rssi, int64_t now)
{
    if (t->probe == TXP_LEAVING && reason == REASON_ASSOC_LEAVE) {
        t->probe = TXP_JOINING;
        return TXT_OURS;
    }
    if (t->probe != TXP_IDLE) return up_fail(t, now);
    if (reason == REASON_4WAY_TIMEOUT || reason == REASON_AUTH_FAIL || reason == REASON_HANDSHAKE_TIMEOUT)
        t->bad_pass = true;
    if (reason != REASON_AUTH_EXPIRE || rssi < t->p.min_rssi) return 0;
    t->no_answer = true;               /* shown even with autotune off */
    if (t->fixed) return 0;
    if (++t->auth_exp < t->p.fails || t->cur <= t->p.floor) return 0;
    t->auth_exp = 0;
    t->cur      = t->cur - t->p.step < t->p.floor ? t->p.floor : t->cur - t->p.step;
    t->downs++;
    plan_up(t, now, t->p.up_every_ms);
    return TXT_APPLY | TXT_LOWERED;
}

int tx_tune_got_ip(tx_tune_t *t, int64_t now)
{
    t->auth_exp  = 0;
    t->no_answer = t->bad_pass = false;
    if (t->probe == TXP_JOINING) {
        t->probe      = TXP_WATCH;
        t->watch_from = now;
        t->ok = t->fail_run = 0;
        return 0;
    }
    if (t->probe != TXP_IDLE || t->fixed || t->cur >= t->cap || t->cur == t->learned) return 0;
    t->learned = t->cur;
    return TXT_SAVE;
}

int tx_tune_fetch(tx_tune_t *t, bool reached_server, int64_t now)
{
    if (t->probe != TXP_WATCH) return 0;
    if (!reached_server) {
        if (++t->fail_run >= t->p.up_fail_run) return up_fail(t, now);
        return 0;
    }
    t->fail_run = 0;
    if (++t->ok < t->p.up_ok_min || now - t->watch_from < t->p.up_watch_ms) return 0;
    t->probe = TXP_IDLE;
    t->ups++;
    if (t->cur >= t->cap) {
        t->learned = 0;
        t->next_up_ms = 0;
        return TXT_ERASE;
    }
    t->learned = t->cur;
    plan_up(t, now, t->p.up_every_ms);
    return TXT_SAVE;
}

int tx_tune_tick(tx_tune_t *t, bool connected, int64_t now)
{
    if ((t->probe == TXP_LEAVING || t->probe == TXP_JOINING) && now - t->probe_at >= t->p.up_join_ms)
        return up_fail(t, now);
    if (t->probe != TXP_IDLE || !connected || !t->next_up_ms || now < t->next_up_ms) return 0;
    if (t->fixed || t->cur >= t->cap) {
        t->next_up_ms = 0;
        return 0;
    }
    t->prev  = t->cur;
    t->cur  += 1;
    t->probe = TXP_LEAVING;
    t->probe_at = now;
    return TXT_APPLY | TXT_RECONNECT;
}

tx_led_t tx_tune_led(const tx_tune_t *t)
{
    if (t->bad_pass) return TXL_BAD_PASS;
    if (!t->no_answer) return TXL_NONE;
    return (t->fixed || t->cur <= t->p.floor) ? TXL_NO_ANSWER_FLOOR : TXL_NO_ANSWER;
}
