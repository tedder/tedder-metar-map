/*
 * Airport list and weather reply parsing. Plain C (no ESP-IDF), so it builds on the host:
 * test/test_airports.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LED_MAX 200

/* One LED of the map, in strip order. icao "" = a dark LED (OFF, NULL, OFF_LEGEND, -). */
typedef struct {
    char    icao[5];
    char    cat[6];        /* server flight category: VFR/MVFR/IFR/LIFR, "" = no report */
    uint8_t r, g, b;
    int16_t age_min;       /* report age from the server, -1 unknown */
    bool    stale_remote;
    bool    has;           /* in the last weather reply */
} slot_t;

/* One line of the metar-service CSV reply. */
typedef struct {
    char    icao[5];
    char    cat[6];
    uint8_t r, g, b;
    int16_t age_min;
    bool    stale;
} csv_row_t;

/** Airport list text -> LED slots, in strip order. Tokens are separated by newlines, commas or
 *  spaces and uppercased; an airport is 3-4 letters/digits, OFF/OFF_xxx/NULL/- is a dark LED.
 *  Returns false (err says why) on a bad token or more than LED_MAX entries. */
bool airports_parse(const char *text, slot_t *out, int *n, char *err, size_t errsz);

/** Canonical text for NVS: one token per line, dark LEDs as OFF. */
void airports_format(const slot_t *slots, int n, char *buf, size_t sz);

/** Comma-separated distinct airports from the list (dark LEDs and repeats skipped). Returns the
 *  number of airports written. */
int airport_ids(const slot_t *slots, int n, char *buf, size_t sz);

/** One CSV line: icao,cat,r-g-b,age_mins,stale[,wdir,wspd,wgst]. Modifies line. */
bool parse_csv_line(char *line, csv_row_t *out);

/** Apply a whole reply (one line per airport) to the slots: every LED with that airport gets its
 *  color; LEDs not in the reply get has = false. Modifies body. Returns rows parsed; *reports =
 *  rows with a current category (category set, not stale). */
int airports_apply_reply(slot_t *slots, int n, char *body, int *reports);
