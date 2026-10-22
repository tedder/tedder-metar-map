/* Host tests for main/radio_verdict.c (make -C test). Numbers are real wifiperf results from one
 * QT Py ESP32-S2 board measured with three different antennas. */
#include <stdio.h>
#include <string.h>

#include "../main/radio_verdict.h"

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
    } while (0)

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static rc_result_t judge(const rc_row_t *r, int n, const char *want_in_why)
{
    char why[160];
    rc_result_t v = rc_verdict(r, n, why, sizeof(why));
    if (want_in_why && !strstr(why, want_in_why)) {
        printf("FAIL why %s: \"%s\" lacks \"%s\"\n", rc_result_name(v), why, want_in_why);
        failures++;
    }
    return v;
}

/* {dbm, sent, recv, avg, max, up, dropped}; 8 dBm rows are the measured 8.5 dBm runs */
static void test_antenna2_near_pass(void)
{
    rc_row_t r[] = { { 17, 50, 50, 9, 50, 455, 0 }, { 11, 50, 50, 8, 50, 476, 0 }, { 8, 50, 50, 8, 50, 421, 0 },
                     { 17, 50, 50, 6, 50, 396, 0 }, { 11, 50, 50, 5, 50, 414, 0 }, { 8, 50, 50, 6, 50, 441, 0 } };
    CHECK(judge(r, N(r), "Good at every level") == RC_PASS);
}

/* One slow 11 dBm round (281) and one fast (483): best of two keeps it a pass, in either order. */
static void test_antenna2_far_noisy_round_pass(void)
{
    rc_row_t r[] = { { 17, 50, 47, 9, 50, 425, 0 }, { 11, 50, 49, 2, 50, 281, 0 }, { 8, 50, 50, 3, 50, 539, 0 },
                     { 17, 50, 50, 4, 50, 568, 0 }, { 11, 50, 50, 4, 50, 483, 0 }, { 8, 50, 50, 4, 50, 490, 0 } };
    CHECK(judge(r, N(r), NULL) == RC_PASS);
    rc_row_t slow_last[] = { r[3], r[4], r[5], r[0], r[1], r[2] };
    CHECK(judge(slow_last, N(slow_last), NULL) == RC_PASS);
}

static void test_antenna2_mid_pass(void)
{
    rc_row_t r[] = { { 17, 50, 50, 9, 50, 264, 0 }, { 11, 50, 50, 9, 50, 227, 0 }, { 8, 50, 50, 45, 300, 236, 0 },
                     { 17, 50, 50, 8, 50, 246, 0 }, { 11, 50, 50, 12, 50, 61, 0 }, { 8, 50, 50, 9, 50, 221, 0 } };
    CHECK(judge(r, N(r), NULL) == RC_PASS);
}

/* Antenna 1 (the bad one): fine at 11 and 8, slow at 17 -> warn, ships at 11. */
static void test_antenna1_near_warn(void)
{
    rc_row_t r[] = { { 17, 50, 50, 30, 100, 62, 0 }, { 11, 50, 50, 6, 30, 490, 0 }, { 8, 50, 50, 5, 30, 485, 0 },
                     { 17, 50, 49, 31, 100, 53, 0 }, { 11, 50, 50, 8, 30, 480, 0 }, { 8, 50, 50, 8, 30, 453, 0 } };
    CHECK(judge(r, N(r), "Weak at high power: pings take 30 ms at 17 dBm") == RC_WARN);
}

static void test_antenna1_mid_upload_warn(void)
{
    rc_row_t r[] = { { 17, 50, 50, 20, 82, 29, 0 }, { 11, 50, 50, 16, 50, 148, 0 }, { 8, 50, 50, 18, 50, 189, 0 } };
    CHECK(judge(r, N(r), "upload 29 kB/s at 17 dBm but 148 at 11") == RC_WARN);
}

static void test_high_power_loss_and_drop_warn(void)
{
    rc_row_t loss[] = { { 17, 50, 1, 96, 96, 0, 0 }, { 11, 50, 50, 6, 30, 490, 0 }, { 8, 50, 50, 5, 30, 485, 0 } };
    rc_row_t drop[] = { { 17, 20, 3, 80, 200, -1, 34 }, { 11, 20, 20, 6, 30, -1, 0 }, { 8, 20, 20, 6, 30, -1, 0 } };
    CHECK(judge(loss, N(loss), "lost 49 of 50 pings at 17 dBm") == RC_WARN);
    CHECK(judge(drop, N(drop), "dropped at 17 dBm (reason 34)") == RC_WARN);
}

static void test_normal_level_fail(void)
{
    rc_row_t drop[] = { { 17, 20, 20, 6, 30, -1, 0 }, { 11, 20, 3, 80, 200, -1, 34 }, { 8, 20, 20, 6, 30, -1, 0 } };
    rc_row_t loss[] = { { 17, 20, 20, 6, 30, -1, 0 }, { 11, 20, 20, 6, 30, -1, 0 }, { 8, 20, 15, 6, 30, -1, 0 } };
    rc_row_t slow[] = { { 17, 20, 20, 6, 30, 50, 0 }, { 11, 20, 20, 6, 30, 60, 0 }, { 8, 20, 20, 6, 30, 400, 0 } };
    rc_row_t ping[] = { { 17, 20, 20, 60, 90, -1, 0 }, { 11, 20, 20, 40, 90, -1, 0 }, { 8, 20, 20, 9, 30, -1, 0 } };
    CHECK(judge(drop, N(drop), "dropped at 11 dBm (reason 34)") == RC_FAIL);
    CHECK(judge(loss, N(loss), "Lost 5 of 20 pings at 8 dBm") == RC_FAIL);
    CHECK(judge(slow, N(slow), "upload 60 kB/s at 11 dBm but 400 at 8") == RC_FAIL);
    CHECK(judge(ping, N(ping), "pings take 40 ms at 11 dBm but 9 ms at 8") == RC_FAIL);
}

static void test_no_sink_pings_pass(void)
{
    rc_row_t r[] = { { 17, 20, 20, 6, 30, -1, 0 }, { 11, 20, 20, 7, 30, -1, 0 }, { 8, 20, 19, 6, 30, -1, 0 } };
    CHECK(judge(r, N(r), "no sink") == RC_PASS);
}

static void test_loss_at_threshold(void)
{
    /* 2 of 20 lost = 10 %: allowed; 3 of 20: fail */
    rc_row_t ok[]  = { { 11, 20, 18, 6, 30, -1, 0 }, { 8, 20, 20, 6, 30, -1, 0 } };
    rc_row_t bad[] = { { 11, 20, 17, 6, 30, -1, 0 }, { 8, 20, 20, 6, 30, -1, 0 } };
    CHECK(judge(ok, N(ok), NULL) == RC_PASS);
    CHECK(judge(bad, N(bad), "Lost 3 of 20") == RC_FAIL);
}

static void test_slow_everywhere_unsure(void)
{
    rc_row_t r[] = { { 17, 20, 20, 9, 30, 30, 0 }, { 11, 20, 20, 9, 30, 40, 0 }, { 8, 20, 20, 9, 30, 45, 0 } };
    CHECK(judge(r, N(r), "move closer") == RC_UNSURE);
    /* too slow to judge even when 11 is under half of 8: unsure, not fail */
    rc_row_t halved[] = { { 17, 20, 20, 9, 30, 20, 0 }, { 11, 20, 20, 9, 30, 15, 0 }, { 8, 20, 20, 9, 30, 40, 0 } };
    CHECK(judge(halved, N(halved), "move closer") == RC_UNSURE);
}

static void test_missing_levels_unsure(void)
{
    /* antenna 3 mid quick run had no 8 dBm step */
    rc_row_t no8[] = { { 17, 50, 50, 21, 85, 54, 0 }, { 11, 50, 50, 19, 115, 146, 0 } };
    CHECK(judge(NULL, 0, "No measurements") == RC_UNSURE);
    CHECK(judge(no8, N(no8), "Not enough levels") == RC_UNSURE);
}

static void test_sink_ok(void)
{
    CHECK(rc_sink_ok("http://192.168.0.10:8399"));
    CHECK(rc_sink_ok("http://10.0.0.1"));
    CHECK(rc_sink_ok("http://172.16.0.1:80"));
    CHECK(rc_sink_ok("http://172.31.255.255:65535"));
    CHECK(!rc_sink_ok("http://172.32.0.1"));
    CHECK(!rc_sink_ok("http://8.8.8.8:80"));
    CHECK(!rc_sink_ok("https://192.168.0.10"));
    CHECK(!rc_sink_ok("http://192.168.0.10/up"));
    CHECK(!rc_sink_ok("http://192.168.0.10:0"));
    CHECK(!rc_sink_ok("http://192.168.0.10:65536"));
    CHECK(!rc_sink_ok("http://192.168.0.256"));
    CHECK(!rc_sink_ok("http://192.168.0"));
    CHECK(!rc_sink_ok("http://192.168.0.10:80x"));
    CHECK(!rc_sink_ok("http://example.com"));
    CHECK(!rc_sink_ok(""));
    CHECK(!rc_sink_ok(NULL));
}

int main(void)
{
    test_sink_ok();
    test_antenna2_near_pass();
    test_antenna2_far_noisy_round_pass();
    test_antenna2_mid_pass();
    test_antenna1_near_warn();
    test_antenna1_mid_upload_warn();
    test_high_power_loss_and_drop_warn();
    test_normal_level_fail();
    test_no_sink_pings_pass();
    test_loss_at_threshold();
    test_slow_everywhere_unsure();
    test_missing_levels_unsure();
    printf("radio_verdict: %s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
