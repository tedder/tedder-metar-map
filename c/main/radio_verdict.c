#include "radio_verdict.h"
#include <stdio.h>
#include <string.h>

#define LOSS_PCT_MAX   10     /* more lost pings than this at one level = fail */
#define SLOW_LINK_KBPS 50     /* best upload below this everywhere: too far from the AP to judge */

const char *rc_result_name(rc_result_t r)
{
    return r == RC_PASS ? "pass" : r == RC_FAIL ? "fail" : r == RC_WARN ? "warn" : "unsure";
}

typedef struct { int sent, recv, avg_sum, avg_n, best_up, dropped; } lvl_t;

static int lvl_avg(const lvl_t *l) { return l->avg_n ? l->avg_sum / l->avg_n : 0; }

static bool lossy(const lvl_t *l) { return l->sent > 0 && (l->sent - l->recv) * 100 > l->sent * LOSS_PCT_MAX; }

/* `hi` worse than `lo`: pings over twice as slow (+10 ms), or best upload under half. */
static bool worse(const lvl_t *hi, const lvl_t *lo, char *why, size_t why_sz, int dhi, int dlo)
{
    int ah = lvl_avg(hi), al = lvl_avg(lo);
    if (al > 0 && ah > 2 * al + 10) {
        snprintf(why, why_sz, "pings take %d ms at %d dBm but %d ms at %d dBm", ah, dhi, al, dlo);
        return true;
    }
    if (hi->best_up >= 0 && lo->best_up >= SLOW_LINK_KBPS && hi->best_up * 2 < lo->best_up) {
        snprintf(why, why_sz, "upload %d kB/s at %d dBm but %d at %d dBm", hi->best_up, dhi, lo->best_up, dlo);
        return true;
    }
    return false;
}

rc_result_t rc_verdict(const rc_row_t *rows, int n, char *why, size_t why_sz)
{
    /* index 0 = 17 dBm (highest allowed), 1 = 11 (default), 2 = 8 (autotune floor) */
    static const int dbm[RC_LEVELS] = { 17, 11, 8 };
    lvl_t L[RC_LEVELS] = { 0 };
    for (int k = 0; k < RC_LEVELS; k++) L[k].best_up = -1;
    if (n <= 0) {
        snprintf(why, why_sz, "No measurements.");
        return RC_UNSURE;
    }
    for (int i = 0; i < n; i++) {
        const rc_row_t *r = &rows[i];
        int k = r->dbm >= 14 ? 0 : r->dbm >= 10 ? 1 : 2;
        if (r->dropped && !L[k].dropped) L[k].dropped = r->dropped;
        L[k].sent += r->sent;
        L[k].recv += r->recv;
        if (r->recv > 0) { L[k].avg_sum += r->avg_ms; L[k].avg_n++; }
        if (r->up_kBps > L[k].best_up) L[k].best_up = r->up_kBps;
    }
    /* The board's own levels (11 default, 8 floor) must be good. */
    for (int k = 1; k < RC_LEVELS; k++) {
        if (L[k].dropped) {
            snprintf(why, why_sz, "The WiFi link dropped at %d dBm (reason %d): antenna or board suspect.",
                     dbm[k], L[k].dropped);
            return RC_FAIL;
        }
        if (lossy(&L[k])) {
            snprintf(why, why_sz, "Lost %d of %d pings at %d dBm: antenna or board suspect.",
                     L[k].sent - L[k].recv, L[k].sent, dbm[k]);
            return RC_FAIL;
        }
    }
    if (L[1].sent == 0 || L[2].sent == 0) {
        snprintf(why, why_sz, "Not enough levels measured (11 and 8 dBm needed).");
        return RC_UNSURE;
    }
    char d[100];
    if (worse(&L[1], &L[2], d, sizeof(d), 11, 8)) {
        snprintf(why, why_sz, "Worse at the normal level: %s. Antenna or board suspect.", d);
        return RC_FAIL;
    }
    int up_lo = L[1].best_up > L[2].best_up ? L[1].best_up : L[2].best_up;
    if (L[1].best_up >= 0 && up_lo < SLOW_LINK_KBPS) {
        snprintf(why, why_sz, "Uploads are slow at every level (best %d kB/s): move closer to the "
                 "access point and run it again.", up_lo);
        return RC_UNSURE;
    }
    /* 17 dBm only warns: units run at 11 unless set by hand. */
    const char *tail = " Fine at 11 dBm (the default); leave wifi_tx_power_dbm unset.";
    if (L[0].dropped) {
        snprintf(why, why_sz, "Weak at high power: the link dropped at 17 dBm (reason %d).%s", L[0].dropped, tail);
        return RC_WARN;
    }
    if (lossy(&L[0])) {
        snprintf(why, why_sz, "Weak at high power: lost %d of %d pings at 17 dBm.%s", L[0].sent - L[0].recv,
                 L[0].sent, tail);
        return RC_WARN;
    }
    if (L[0].sent > 0 && worse(&L[0], &L[1], d, sizeof(d), 17, 11)) {
        snprintf(why, why_sz, "Weak at high power: %s.%s", d, tail);
        return RC_WARN;
    }
    if (L[1].best_up < 0)
        snprintf(why, why_sz, "Pings fine at every level (no upload test: no sink given).");
    else
        snprintf(why, why_sz, "Good at every level (upload %d kB/s at 11 dBm, %d at 17).", L[1].best_up,
                 L[0].best_up);
    return RC_PASS;
}

/* One decimal number 0..max at *p; advances p. */
static bool num(const char **p, unsigned max, unsigned *out)
{
    const char *s = *p;
    unsigned v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 6) { v = v * 10 + (unsigned)(*s - '0'); s++; digits++; }
    if (digits == 0 || digits > 5 || v > max) return false;
    *p  = s;
    *out = v;
    return true;
}

bool rc_sink_ok(const char *url)
{
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;
    unsigned o[4], port;
    for (int i = 0; i < 4; i++) {
        if (!num(&p, 255, &o[i])) return false;
        if (i < 3 && *p++ != '.') return false;
    }
    if (*p == ':') {
        p++;
        if (!num(&p, 65535, &port) || port == 0) return false;
    }
    if (*p != '\0') return false;
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
}
