/*
 * ctydat.c - Country/prefix database parser and lookup
 *
 * Parses AD1C's cty.dat / bigcty.dat format. The file consists
 * of entity records, each with a header line followed by prefix
 * lines. We build a simple hash table of prefix → country for
 * fast longest-prefix-match lookups.
 *
 * File format reference: https://www.country-files.com/
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ctydat.h"
#include "callsign_districts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Simple hash table for prefix lookups.
 * We use a fixed-size table with chaining. BigCTY has ~50,000
 * prefix entries, so 16K buckets gives reasonable load factor.
 */
#define HASH_BUCKETS 16384
#define MAX_COUNTRIES 400
#define MAX_PREFIX_LEN 16

typedef struct prefix_entry {
    char prefix[MAX_PREFIX_LEN];
    int country_idx;            /* Index into countries array */
    double lat, lon;            /* Per-prefix coordinate override */
    int has_override;           /* Non-zero if lat/lon are set   */
    struct prefix_entry *next;  /* Chain for hash collisions     */
} prefix_entry_t;

struct pic_ctydat {
    pic_country_t countries[MAX_COUNTRIES];
    int num_countries;
    prefix_entry_t *buckets[HASH_BUCKETS];
};

/*
 * hash_str - Hash a prefix string using the djb2 algorithm.
 *
 * djb2 (Daniel J. Bernstein) is chosen for simplicity and good
 * distribution across typical short alphanumeric strings like
 * callsign prefixes. Case-insensitive: converts to uppercase
 * so "vk" and "VK" hash to the same bucket.
 */
static unsigned int hash_str(const char *s)
{
    unsigned int h = 5381;
    while (*s) {
        h = ((h << 5) + h) + toupper((unsigned char)*s);
        s++;
    }
    return h % HASH_BUCKETS;
}

/*
 * add_prefix - Insert a prefix → country mapping into the hash table.
 *
 * cty.dat prefixes may carry inline annotations that override the
 * parent entity's zone/location for that specific prefix:
 *   =VK9XY(28)[51]  — exact callsign match with CQ zone 28, ITU 51
 *   VP2M(11)         — prefix with CQ zone override
 *   <43.2/-2.5>      — lat/lon override
 *   {33}             — country override
 *
 * We parse <lat/lon> overrides for per-district geolocation
 * (BigCTY provides these for callsign districts in large countries).
 * CQ/ITU zone and grid overrides are stripped. The '=' exact-match
 * marker is stripped from the prefix string.
 *
 * BigCTY uses West-positive longitude in overrides (same as the
 * header) — we negate it to East-positive for the renderer.
 */
static void add_prefix(pic_ctydat_t *db, const char *prefix, int country_idx)
{
    unsigned int h;
    prefix_entry_t *entry;
    char clean[MAX_PREFIX_LEN];
    double override_lat = 0, override_lon = 0;
    int has_override = 0;
    int i, j;

    j = 0;
    for (i = 0; prefix[i] && j < MAX_PREFIX_LEN - 1; i++) {
        char c = prefix[i];
        if (c == '<') {
            /* Parse <lat/lon> coordinate override */
            if (sscanf(prefix + i, "<%lf/%lf>",
                       &override_lat, &override_lon) == 2) {
                override_lon = -override_lon;
                has_override = 1;
            }
            break;
        }
        if (c == '(' || c == '[' || c == '{') break;
        if (c == '=') continue;
        clean[j++] = toupper((unsigned char)c);
    }
    clean[j] = '\0';

    if (j == 0) return;

    entry = calloc(1, sizeof(*entry));
    if (!entry) return;

    strncpy(entry->prefix, clean, MAX_PREFIX_LEN - 1);
    entry->country_idx = country_idx;
    entry->has_override = has_override;
    entry->lat = override_lat;
    entry->lon = override_lon;

    h = hash_str(clean);
    entry->next = db->buckets[h];
    db->buckets[h] = entry;
}

/*
 * Parse a header line:
 *   Country Name:  CQ: ITU: Cont: Lat: Lon: UTC: Prefix:
 * Fields are colon-separated, with leading/trailing whitespace.
 */
static int parse_header(const char *line, pic_country_t *country)
{
    char fields[8][128];
    int field = 0;
    int i = 0, j = 0;

    memset(fields, 0, sizeof(fields));

    /* Split by colon */
    while (line[i] && field < 8) {
        if (line[i] == ':') {
            fields[field][j] = '\0';
            field++;
            j = 0;
            i++;
            continue;
        }
        if (j < 127) {
            fields[field][j++] = line[i];
        }
        i++;
    }

    if (field < 8) return -1;

    /* Trim whitespace from each field */
    for (i = 0; i < 8; i++) {
        char *s = fields[i];
        while (*s == ' ') s++;
        char *e = s + strlen(s) - 1;
        while (e > s && *e == ' ') *e-- = '\0';
        memmove(fields[i], s, strlen(s) + 1);
    }

    strncpy(country->name, fields[0], sizeof(country->name) - 1);
    country->cq_zone = atoi(fields[1]);
    country->itu_zone = atoi(fields[2]);
    strncpy(country->continent, fields[3], sizeof(country->continent) - 1);
    country->lat = atof(fields[4]);
    /* Convert West-positive longitude to East-positive */
    country->lon = -atof(fields[5]);
    country->utc_offset = atof(fields[6]);
    strncpy(country->prefix, fields[7], sizeof(country->prefix) - 1);

    return 0;
}

/*
 * pic_ctydat_load - Parse a cty.dat file into a prefix hash table.
 *
 * The file alternates between header lines (country metadata) and
 * prefix lines (indented, comma-separated). A semicolon ends the
 * prefix list for each entity.
 *
 *   Header:  "Country Name: CQ: ITU: Cont: Lat: Lon: UTC: Prefix:"
 *   Prefix:  "    AB1,AB2,=AB1XY(zone),AB3;"
 *
 * BigCTY typically has ~350 entities and ~50,000 prefix entries.
 * With 16K hash buckets the average chain length is ~3.
 */
pic_ctydat_t *pic_ctydat_load(const char *path)
{
    FILE *f;
    pic_ctydat_t *db;
    char line[4096];
    int current_country = -1;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ctydat: cannot open '%s'\n", path);
        return NULL;
    }

    db = calloc(1, sizeof(*db));
    if (!db) {
        fclose(f);
        return NULL;
    }

    printf("ctydat: loading '%s'\n", path);

    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline/whitespace */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' ')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;

        /*
         * Detect header vs prefix line:
         * Header lines start with a non-space character.
         * Prefix lines start with spaces.
         */
        if (line[0] != ' ' && line[0] != '\t') {
            /* Header line — new country */
            if (db->num_countries >= MAX_COUNTRIES) continue;

            current_country = db->num_countries;
            if (parse_header(line, &db->countries[current_country]) == 0) {
                /* Add the primary prefix */
                add_prefix(db, db->countries[current_country].prefix,
                          current_country);
                db->num_countries++;
            } else {
                current_country = -1;
            }
        } else if (current_country >= 0) {
            /* Prefix line — comma-separated prefixes, ends with ; */
            char *p = line;
            char prefix[64];
            int pi = 0;

            while (*p) {
                if (*p == ',' || *p == ';') {
                    prefix[pi] = '\0';
                    /* Trim whitespace */
                    char *s = prefix;
                    while (*s == ' ' || *s == '\t') s++;
                    if (*s) {
                        add_prefix(db, s, current_country);
                    }
                    pi = 0;
                    if (*p == ';') break; /* End of this entity's prefixes */
                } else {
                    if (pi < 63) prefix[pi++] = *p;
                }
                p++;
            }
        }
    }

    fclose(f);
    printf("ctydat: loaded %d countries\n", db->num_countries);

    return db;
}

/*
 * pic_ctydat_lookup - Find the DXCC entity for a callsign.
 *
 * Uses longest-prefix-match: tries the full callsign first, then
 * progressively shorter prefixes until a match is found. Stops
 * at the first '/' to strip portable/mobile indicators (/P, /M,
 * /QRP etc.) since the country is determined by the base call.
 *
 * Example: "VK2ABC/P" → tries "VK2ABC", "VK2AB", "VK2A", "VK2",
 * "VK", "V" — matches "VK" → Australia.
 */
const pic_country_t *pic_ctydat_lookup(const pic_ctydat_t *db,
                                       const char *callsign)
{
    char upper[32];
    int len, i;

    if (!db || !callsign) return NULL;

    /* Uppercase the callsign, strip /P /M etc. */
    for (i = 0; callsign[i] && i < 31; i++) {
        if (callsign[i] == '/') break; /* Stop at first / */
        upper[i] = toupper((unsigned char)callsign[i]);
    }
    upper[i] = '\0';
    len = i;

    /* Longest prefix match: try full callsign, then strip from right */
    for (i = len; i > 0; i--) {
        unsigned int h;
        prefix_entry_t *entry;
        char try[32];

        memcpy(try, upper, i);
        try[i] = '\0';

        h = hash_str(try);
        for (entry = db->buckets[h]; entry; entry = entry->next) {
            if (strcmp(entry->prefix, try) == 0) {
                return &db->countries[entry->country_idx];
            }
        }
    }

    return NULL;
}

/*
 * pic_ctydat_lookup_location - Look up a callsign and return coordinates.
 *
 * Like pic_ctydat_lookup but also returns the best-available lat/lon.
 * If the matched prefix has a <lat/lon> override from BigCTY (e.g.,
 * per-district coordinates for US callsign areas), those are returned.
 * Otherwise the parent entity coordinates are used.
 *
 * Returns the country pointer (same as pic_ctydat_lookup), or NULL.
 */
const pic_country_t *pic_ctydat_lookup_location(const pic_ctydat_t *db,
                                                 const char *callsign,
                                                 double *out_lat,
                                                 double *out_lon)
{
    char upper[32];
    int len, i;

    if (!db || !callsign) return NULL;

    for (i = 0; callsign[i] && i < 31; i++) {
        if (callsign[i] == '/') break;
        upper[i] = toupper((unsigned char)callsign[i]);
    }
    upper[i] = '\0';
    len = i;

    for (i = len; i > 0; i--) {
        unsigned int h;
        prefix_entry_t *entry;
        char try[32];

        memcpy(try, upper, i);
        try[i] = '\0';

        h = hash_str(try);
        for (entry = db->buckets[h]; entry; entry = entry->next) {
            if (strcmp(entry->prefix, try) == 0) {
                const pic_country_t *c = &db->countries[entry->country_idx];
                if (entry->has_override) {
                    /* Per-prefix override from BigCTY <lat/lon> */
                    if (out_lat) *out_lat = entry->lat;
                    if (out_lon) *out_lon = entry->lon;
                } else if (pic_district_lookup(callsign, out_lat, out_lon)) {
                    /* District table matched a more specific prefix */
                } else {
                    /* Fall back to country centre */
                    if (out_lat) *out_lat = c->lat;
                    if (out_lon) *out_lon = c->lon;
                }
                return c;
            }
        }
    }

    return NULL;
}

/*
 * pic_ctydat_free - Release the prefix database and all hash chains.
 */
void pic_ctydat_free(pic_ctydat_t *db)
{
    int i;

    if (!db) return;

    /* Walk every bucket and free the collision chains */
    for (i = 0; i < HASH_BUCKETS; i++) {
        prefix_entry_t *entry = db->buckets[i];
        while (entry) {
            prefix_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    free(db);
}
