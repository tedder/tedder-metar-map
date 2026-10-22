#include "airports.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Dark-LED tokens: OFF, OFF_anything (OFF_LEGEND), NULL, "-". */
static bool token_dark(const char *t)
{
    return strncmp(t, "OFF", 3) == 0 || strcmp(t, "NULL") == 0 || strcmp(t, "-") == 0;
}

static bool token_airport(const char *t)
{
    size_t n = strlen(t);
    if (n < 3 || n > 4) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)t[i])) return false;
    return true;
}

bool airports_parse(const char *text, slot_t *out, int *n, char *err, size_t errsz)
{
    int k = 0;
    const char *p = text ? text : "";
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char tok[16];
        size_t L = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ',') {
            if (L + 1 < sizeof(tok)) tok[L++] = (char)toupper((unsigned char)*p);
            p++;
        }
        tok[L] = '\0';
        if (k >= LED_MAX) { snprintf(err, errsz, "more than %d entries", LED_MAX); return false; }
        bool dark = token_dark(tok);
        if (!dark && !token_airport(tok)) {
            snprintf(err, errsz, "LED %d: \"%.12s\" is not an airport code or OFF", k + 1, tok);
            return false;
        }
        slot_t *s = &out[k++];
        memset(s, 0, sizeof(*s));
        s->age_min = -1;
        if (!dark) snprintf(s->icao, sizeof(s->icao), "%.4s", tok);
    }
    *n = k;
    if (errsz) err[0] = '\0';
    return true;
}

void airports_format(const slot_t *slots, int n, char *buf, size_t sz)
{
    size_t off = 0;
    if (!sz) return;
    buf[0] = '\0';
    for (int i = 0; i < n && off + 6 < sz; i++)
        off += (size_t)snprintf(buf + off, sz - off, "%s%s", i ? "\n" : "",
                                slots[i].icao[0] ? slots[i].icao : "OFF");
}

int airport_ids(const slot_t *slots, int n, char *buf, size_t sz)
{
    size_t off = 0;
    int k = 0;
    if (!sz) return 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (!slots[i].icao[0]) continue;
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = strcmp(slots[j].icao, slots[i].icao) == 0;
        if (dup || off + 6 >= sz) continue;
        off += (size_t)snprintf(buf + off, sz - off, "%s%s", k ? "," : "", slots[i].icao);
        k++;
    }
    return k;
}

bool parse_csv_line(char *line, csv_row_t *out)
{
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';
    char *fields[5] = { 0 };
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < 5) {
        if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
        p++;
    }
    if (n < 5) return false;
    char *e = strchr(fields[4], ',');
    if (e) *e = '\0';
    int r, g, b;
    if (sscanf(fields[2], "%d-%d-%d", &r, &g, &b) != 3) return false;
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return false;
    size_t il = strlen(fields[0]);
    if (il < 3 || il > 4) return false;
    snprintf(out->icao, sizeof(out->icao), "%.4s", fields[0]);
    snprintf(out->cat, sizeof(out->cat), "%.5s", fields[1]);
    out->r = (uint8_t)r; out->g = (uint8_t)g; out->b = (uint8_t)b;
    out->age_min = fields[3][0] ? (int16_t)atoi(fields[3]) : -1;
    out->stale = fields[4][0] == '1';
    return true;
}

int airports_apply_reply(slot_t *slots, int n, char *body, int *reports)
{
    for (int i = 0; i < n; i++) slots[i].has = false;
    int rows = 0, rep = 0;
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        csv_row_t row;
        if (!parse_csv_line(line, &row)) continue;
        rows++;
        if (row.cat[0] && !row.stale) rep++;
        for (int i = 0; i < n; i++) {
            slot_t *s = &slots[i];
            if (strcmp(s->icao, row.icao) != 0) continue;
            memcpy(s->cat, row.cat, sizeof(s->cat));
            s->r = row.r; s->g = row.g; s->b = row.b;
            s->age_min = row.age_min;
            s->stale_remote = row.stale;
            s->has = true;
        }
    }
    if (reports) *reports = rep;
    return rows;
}
