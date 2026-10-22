/* Host tests for main/airports.c: airport list parsing and the weather reply. */
#include <stdio.h>
#include <string.h>

#include "../main/airports.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static slot_t sl[LED_MAX + 5];

static void test_parse_basic(void)
{
    int n = -1;
    char err[80];
    CHECK(airports_parse("kpdx\nKTTD\nOFF\n  s48 \r\nOFF_LEGEND,NULL - KHIO", sl, &n, err, sizeof(err)));
    CHECK(n == 8);
    CHECK(strcmp(sl[0].icao, "KPDX") == 0);
    CHECK(strcmp(sl[1].icao, "KTTD") == 0);
    CHECK(sl[2].icao[0] == '\0');
    CHECK(strcmp(sl[3].icao, "S48") == 0);
    CHECK(sl[4].icao[0] == '\0' && sl[5].icao[0] == '\0' && sl[6].icao[0] == '\0');
    CHECK(strcmp(sl[7].icao, "KHIO") == 0);
    CHECK(sl[0].age_min == -1 && !sl[0].has);
}

static void test_parse_empty(void)
{
    int n = -1;
    char err[80];
    CHECK(airports_parse("", sl, &n, err, sizeof(err)) && n == 0);
    CHECK(airports_parse(NULL, sl, &n, err, sizeof(err)) && n == 0);
    CHECK(airports_parse(" \n\n , ", sl, &n, err, sizeof(err)) && n == 0);
}

static void test_parse_bad(void)
{
    int n = 7;
    char err[80];
    CHECK(!airports_parse("KPDX\nKP\n", sl, &n, err, sizeof(err)));
    CHECK(strstr(err, "LED 2") != NULL);
    CHECK(n == 7);                                         /* untouched on failure */
    CHECK(!airports_parse("KPDXX", sl, &n, err, sizeof(err)));
    CHECK(!airports_parse("K-PD", sl, &n, err, sizeof(err)));
    CHECK(!airports_parse("<script>", sl, &n, err, sizeof(err)));
    /* too many */
    char big[LED_MAX * 6 + 16];
    size_t o = 0;
    for (int i = 0; i <= LED_MAX; i++) o += (size_t)snprintf(big + o, sizeof(big) - o, "OFF\n");
    CHECK(!airports_parse(big, sl, &n, err, sizeof(err)));
    CHECK(strstr(err, "more than") != NULL);
    /* exactly LED_MAX is fine */
    o = 0;
    for (int i = 0; i < LED_MAX; i++) o += (size_t)snprintf(big + o, sizeof(big) - o, "KPDX ");
    CHECK(airports_parse(big, sl, &n, err, sizeof(err)) && n == LED_MAX);
}

static void test_format_roundtrip(void)
{
    int n, n2;
    char err[80], text[256];
    slot_t b[8];
    CHECK(airports_parse("kpdx, null ,S48,OFF_LEGEND", sl, &n, err, sizeof(err)));
    airports_format(sl, n, text, sizeof(text));
    CHECK(strcmp(text, "KPDX\nOFF\nS48\nOFF") == 0);
    CHECK(airports_parse(text, b, &n2, err, sizeof(err)) && n2 == n);
    for (int i = 0; i < n; i++) CHECK(strcmp(b[i].icao, sl[i].icao) == 0);
}

static void test_ids(void)
{
    int n;
    char err[80], ids[64];
    CHECK(airports_parse("KPDX OFF KTTD KPDX S48 OFF KTTD", sl, &n, err, sizeof(err)));
    CHECK(airport_ids(sl, n, ids, sizeof(ids)) == 3);
    CHECK(strcmp(ids, "KPDX,KTTD,S48") == 0);
    CHECK(airport_ids(sl, n, ids, 8) == 1);                /* stops before overflowing */
    CHECK(strcmp(ids, "KPDX") == 0);
    CHECK(airports_parse("OFF OFF", sl, &n, err, sizeof(err)));
    CHECK(airport_ids(sl, n, ids, sizeof(ids)) == 0 && ids[0] == '\0');
}

static void test_csv(void)
{
    csv_row_t r;
    char a[] = "KPDX,VFR,0-127-0,50,0,300,12,\r";
    CHECK(parse_csv_line(a, &r));
    CHECK(strcmp(r.icao, "KPDX") == 0 && strcmp(r.cat, "VFR") == 0);
    CHECK(r.r == 0 && r.g == 127 && r.b == 0 && r.age_min == 50 && !r.stale);
    char b[] = "K1W1,,75-75-50,,1,,,";                     /* no report */
    CHECK(parse_csv_line(b, &r));
    CHECK(r.cat[0] == '\0' && r.stale && r.age_min == -1 && r.r == 75);
    char c[] = "KONP,LIFR,255-22-199,8,0";                 /* no wind fields */
    CHECK(parse_csv_line(c, &r) && r.b == 199 && strcmp(r.cat, "LIFR") == 0);
    char d[] = "garbage";
    CHECK(!parse_csv_line(d, &r));
    char e[] = "KPDX,VFR,0-127,50,0";
    CHECK(!parse_csv_line(e, &r));
    char f[] = "KPDX,VFR,0-300-0,50,0";
    CHECK(!parse_csv_line(f, &r));
    char g[] = "TOOLONG,VFR,0-127-0,50,0";
    CHECK(!parse_csv_line(g, &r));
    char h[] = "<html>,x,1-2-3,4,0";
    CHECK(!parse_csv_line(h, &r));
}

static void test_apply(void)
{
    int n, rep = -1;
    char err[80];
    CHECK(airports_parse("KPDX OFF KTTD KPDX K1W1 S48", sl, &n, err, sizeof(err)));
    char body[] = "KPDX,MVFR,0-0-127,12,0,,,\nKTTD,IFR,127-0-0,5,0\nK1W1,,75-75-50,,1,,,\nJUNK\nKXXX,VFR,0-127-0,1,0\n";
    CHECK(airports_apply_reply(sl, n, body, &rep) == 4);   /* JUNK skipped, KXXX parsed but unused */
    CHECK(rep == 3);                                       /* KPDX KTTD KXXX current; K1W1 stale */
    CHECK(sl[0].has && sl[0].b == 127 && strcmp(sl[0].cat, "MVFR") == 0);
    CHECK(!sl[1].has);                                     /* OFF */
    CHECK(sl[2].has && sl[2].r == 127);
    CHECK(sl[3].has && sl[3].b == 127 && sl[3].age_min == 12);   /* same airport, second LED */
    CHECK(sl[4].has && sl[4].stale_remote && sl[4].cat[0] == '\0');
    CHECK(!sl[5].has);                                     /* S48 not in the reply */
    /* next reply without KTTD: its LED drops out */
    char body2[] = "KPDX,VFR,0-127-0,1,0\n";
    CHECK(airports_apply_reply(sl, n, body2, &rep) == 1 && rep == 1);
    CHECK(sl[0].has && sl[0].g == 127 && !sl[2].has);
    char empty[] = "";
    CHECK(airports_apply_reply(sl, n, empty, &rep) == 0 && rep == 0);
}

int main(void)
{
    test_parse_basic();
    test_parse_empty();
    test_parse_bad();
    test_format_roundtrip();
    test_ids();
    test_csv();
    test_apply();
    printf("airports: %s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
