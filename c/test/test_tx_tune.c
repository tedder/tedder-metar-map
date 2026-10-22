/* Host tests for main/tx_tune.c (make -C test). */
#include <stdio.h>

#include "../main/tx_tune.h"

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
    } while (0)

#define MIN  (60LL * 1000)
#define DAY  (24LL * 60 * MIN)
#define AUTH_EXPIRE 2
#define ASSOC_LEAVE 8
#define NO_AP_FOUND 201

static tx_tune_t fresh(int cap, bool fixed, int saved)
{
    tx_tune_t t;
    tx_tune_init(&t, &TX_TUNE_DEFAULTS, cap, fixed, saved, 0);
    return t;
}

/* Run a passing step-up test starting at `now`; returns the time it ends. */
static int64_t pass_up(tx_tune_t *t, int64_t now)
{
    CHECK(tx_tune_tick(t, true, now) == (TXT_APPLY | TXT_RECONNECT));
    CHECK(tx_tune_disconnect(t, ASSOC_LEAVE, -30, now + 100) == TXT_OURS);
    CHECK(tx_tune_got_ip(t, now + 3000) == 0);
    CHECK(t->probe == TXP_WATCH);
    int a = 0;
    for (int i = 1; i <= 10; i++) a = tx_tune_fetch(t, true, now + 3000 + i * MIN);
    CHECK(t->probe == TXP_IDLE);
    (void)a;
    return now + 3000 + 10 * MIN;
}

static void test_defaults(void)
{
    tx_tune_t t = fresh(17, false, 0);
    CHECK(t.cur == 17 && t.learned == 0 && t.next_up_ms == 0);
    CHECK(tx_tune_tick(&t, true, 30 * DAY) == 0);      /* at the cap: never probes */
    CHECK(tx_tune_got_ip(&t, 1) == 0);                 /* nothing to save */
}

static void test_down_and_learn(void)
{
    tx_tune_t t = fresh(20, false, 0);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -55, 1000) == 0);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -55, 5000) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17 && t.downs == 1);
    CHECK(t.next_up_ms == 5000 + 7 * DAY);
    CHECK(tx_tune_got_ip(&t, 9000) == TXT_SAVE && t.learned == 17);
    CHECK(tx_tune_got_ip(&t, 9500) == 0);              /* already saved */
}

static void test_down_filters(void)
{
    tx_tune_t t = fresh(20, false, 0);
    /* weak signal: maybe just far away, never counts */
    for (int i = 0; i < 5; i++) CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -72, i) == 0);
    CHECK(t.cur == 20);
    /* other reasons (out-of-range network during slot rotation) neither count nor reset */
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -54, 10) == 0);
    for (int i = 0; i < 3; i++) CHECK(tx_tune_disconnect(&t, NO_AP_FOUND, 0, 20 + i) == 0);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -28, 30) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17);
    /* an IP resets the count */
    tx_tune_got_ip(&t, 40);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -40, 50) == 0);
    tx_tune_got_ip(&t, 60);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -40, 70) == 0);
    CHECK(t.cur == 17);
}

static void test_floor(void)
{
    tx_tune_t t = fresh(20, false, 0);
    for (int i = 0; i < 20; i++) tx_tune_disconnect(&t, AUTH_EXPIRE, -40, i);
    CHECK(t.cur == 8);        /* 20, 17, 14, 11, 8 */
    CHECK(t.downs == 4);
}

static void test_fixed(void)
{
    tx_tune_t t = fresh(19, true, 14);
    CHECK(t.cur == 19 && t.learned == 0 && t.next_up_ms == 0);  /* saved level ignored */
    for (int i = 0; i < 10; i++) CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -30, i) == 0);
    CHECK(t.cur == 19);
    CHECK(tx_tune_tick(&t, true, 60 * DAY) == 0);
}

static void test_saved_level(void)
{
    tx_tune_t t = fresh(20, false, 17);
    CHECK(t.cur == 17 && t.learned == 17 && t.next_up_ms == 7 * DAY);
    t = fresh(17, false, 17);   /* not below the cap: ignored */
    CHECK(t.cur == 17 && t.learned == 0);
    t = fresh(17, false, 5);    /* below the floor: ignored */
    CHECK(t.cur == 17 && t.learned == 0);
}

static void test_up_pass_with_server_errors(void)
{
    tx_tune_t t = fresh(20, false, 17);
    CHECK(tx_tune_tick(&t, true, 7 * DAY - 1) == 0);           /* not yet */
    CHECK(tx_tune_tick(&t, false, 7 * DAY) == 0);              /* not while disconnected */
    int64_t now = 7 * DAY;
    CHECK(tx_tune_tick(&t, true, now) == (TXT_APPLY | TXT_RECONNECT));
    CHECK(t.cur == 18 && t.prev == 17 && t.probe == TXP_LEAVING);
    CHECK(tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100) == TXT_OURS);
    CHECK(t.probe == TXP_JOINING);
    CHECK(tx_tune_got_ip(&t, now + 3000) == 0 && t.probe == TXP_WATCH);
    /* server answered 500/503/404: still reached it, so the link works */
    CHECK(tx_tune_fetch(&t, true, now + 1 * MIN) == 0);
    CHECK(tx_tune_fetch(&t, true, now + 2 * MIN) == 0);
    CHECK(tx_tune_fetch(&t, true, now + 3 * MIN) == 0);         /* 3 ok but watch not over */
    /* one transport failure (a slow cache miss) is not enough to fail */
    CHECK(tx_tune_fetch(&t, false, now + 4 * MIN) == 0);
    CHECK(tx_tune_fetch(&t, true, now + 13 * MIN) == TXT_SAVE);
    CHECK(t.cur == 18 && t.learned == 18 && t.ups == 1 && t.probe == TXP_IDLE);
    CHECK(t.next_up_ms == now + 13 * MIN + 7 * DAY);
}

static void test_up_fail_rejoin(void)
{
    tx_tune_t t = fresh(20, false, 18);
    int64_t now = 7 * DAY;
    tx_tune_tick(&t, true, now);
    CHECK(t.cur == 19);
    CHECK(tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100) == TXT_OURS);
    /* board can't auth at 19: first failure goes straight back */
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -30, now + 4500) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 18 && t.probe == TXP_IDLE && t.up_fails == 1);
    CHECK(t.next_up_ms == now + 4500 + 30 * DAY);
    CHECK(t.downs == 0);                        /* a failed test is not a step down */
    CHECK(tx_tune_got_ip(&t, now + 8000) == 0); /* back at the learned level: nothing to save */
}

static void test_up_fail_transport(void)
{
    tx_tune_t t = fresh(20, false, 17);
    int64_t now = 7 * DAY;
    tx_tune_tick(&t, true, now);
    tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100);
    tx_tune_got_ip(&t, now + 3000);
    CHECK(tx_tune_fetch(&t, false, now + 1 * MIN) == 0);
    CHECK(tx_tune_fetch(&t, false, now + 2 * MIN) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17 && t.up_fails == 1);
}

static void test_up_fail_drop_while_watching(void)
{
    tx_tune_t t = fresh(20, false, 17);
    int64_t now = 7 * DAY;
    tx_tune_tick(&t, true, now);
    tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100);
    tx_tune_got_ip(&t, now + 3000);
    tx_tune_fetch(&t, true, now + MIN);
    CHECK(tx_tune_disconnect(&t, 200 /* BEACON_TIMEOUT */, -30, now + 2 * MIN) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17 && t.probe == TXP_IDLE);
}

static void test_up_fail_unexpected_leave_reason(void)
{
    tx_tune_t t = fresh(20, false, 17);
    tx_tune_tick(&t, true, 7 * DAY);
    /* while leaving, a disconnect that isn't our own leave is a failure */
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -30, 7 * DAY + 50) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17);
}

static void test_up_join_timeout(void)
{
    tx_tune_t t = fresh(20, false, 17);
    int64_t now = 7 * DAY;
    tx_tune_tick(&t, true, now);
    tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100);
    CHECK(tx_tune_tick(&t, false, now + 59 * 1000) == 0);
    CHECK(tx_tune_tick(&t, false, now + 60 * 1000) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 17 && t.probe == TXP_IDLE);
}

static void test_up_to_cap(void)
{
    tx_tune_t t = fresh(20, false, 17);
    int64_t now = 7 * DAY;
    for (int lvl = 18; lvl <= 20; lvl++) {
        now = pass_up(&t, t.next_up_ms);
        CHECK(t.cur == lvl);
    }
    CHECK(t.learned == 0 && t.next_up_ms == 0 && t.ups == 3);   /* last pass returned ERASE */
    CHECK(tx_tune_tick(&t, true, now + 30 * DAY) == 0);
}

static void test_up_last_step_erases(void)
{
    tx_tune_t t = fresh(18, false, 17);
    int64_t now = 7 * DAY;
    tx_tune_tick(&t, true, now);
    tx_tune_disconnect(&t, ASSOC_LEAVE, -30, now + 100);
    tx_tune_got_ip(&t, now + 3000);
    int a = 0;
    for (int i = 1; i <= 10; i++) a = tx_tune_fetch(&t, true, now + 3000 + i * MIN);
    CHECK(a == TXT_ERASE && t.cur == 18 && t.learned == 0);
}

static void test_down_after_up(void)
{
    /* passes 18, later the board gets worse: steps down 3 from 18 */
    tx_tune_t t = fresh(20, false, 17);
    int64_t now = pass_up(&t, 7 * DAY);
    CHECK(t.cur == 18);
    tx_tune_disconnect(&t, AUTH_EXPIRE, -40, now + DAY);
    CHECK(tx_tune_disconnect(&t, AUTH_EXPIRE, -40, now + DAY + 5000) == (TXT_APPLY | TXT_LOWERED));
    CHECK(t.cur == 15);
    CHECK(tx_tune_got_ip(&t, now + DAY + 9000) == TXT_SAVE && t.learned == 15);
}

static void test_led(void)
{
    tx_tune_t t = fresh(20, false, 0);
    CHECK(tx_tune_led(&t) == TXL_NONE);
    tx_tune_disconnect(&t, NO_AP_FOUND, 0, 1);
    tx_tune_disconnect(&t, AUTH_EXPIRE, -75, 2);         /* weak: maybe far away, plain white */
    CHECK(tx_tune_led(&t) == TXL_NONE);
    tx_tune_disconnect(&t, AUTH_EXPIRE, -40, 3);
    CHECK(tx_tune_led(&t) == TXL_NO_ANSWER);             /* amber from the first strong timeout */
    for (int i = 0; i < 20; i++) tx_tune_disconnect(&t, AUTH_EXPIRE, -40, 10 + i);
    CHECK(t.cur == 8 && tx_tune_led(&t) == TXL_NO_ANSWER_FLOOR);   /* nothing left to try: blink */
    tx_tune_got_ip(&t, 100);
    CHECK(tx_tune_led(&t) == TXL_NONE);

    tx_tune_disconnect(&t, 15, -40, 200);                /* 4-way handshake timeout */
    CHECK(tx_tune_led(&t) == TXL_BAD_PASS);
    tx_tune_disconnect(&t, AUTH_EXPIRE, -40, 201);       /* password wins over no-answer */
    CHECK(tx_tune_led(&t) == TXL_BAD_PASS);
    tx_tune_got_ip(&t, 300);
    tx_tune_disconnect(&t, 204, -40, 301);
    CHECK(tx_tune_led(&t) == TXL_BAD_PASS);
    tx_tune_got_ip(&t, 400);
    tx_tune_disconnect(&t, 202, -40, 401);               /* WPA3 SAE auth fail */
    CHECK(tx_tune_led(&t) == TXL_BAD_PASS);

    tx_tune_t f = fresh(17, true, 0);                    /* autotune off: no answer = can't help, blink */
    tx_tune_disconnect(&f, AUTH_EXPIRE, -40, 1);
    CHECK(f.cur == 17 && tx_tune_led(&f) == TXL_NO_ANSWER_FLOOR);

    tx_tune_t u = fresh(20, false, 17);                  /* failed step-up test: not a join problem */
    tx_tune_tick(&u, true, 7 * DAY);
    tx_tune_disconnect(&u, ASSOC_LEAVE, -30, 7 * DAY + 1);
    tx_tune_disconnect(&u, AUTH_EXPIRE, -30, 7 * DAY + 2);
    CHECK(u.cur == 17 && tx_tune_led(&u) == TXL_NONE);
}

int main(void)
{
    test_led();
    test_defaults();
    test_down_and_learn();
    test_down_filters();
    test_floor();
    test_fixed();
    test_saved_level();
    test_up_pass_with_server_errors();
    test_up_fail_rejoin();
    test_up_fail_transport();
    test_up_fail_drop_while_watching();
    test_up_fail_unexpected_leave_reason();
    test_up_join_timeout();
    test_up_to_cap();
    test_up_last_step_erases();
    test_down_after_up();
    printf("tx_tune: %s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
