/*
 * callsign_districts.h - Callsign prefix to geographic location
 *
 * Provides district-level geolocation for amateur radio callsigns.
 * Maps numbered prefixes (W1, VE3, VK6, etc.) to the geographic
 * centre of the callsign district, giving much better map placement
 * than the country-centre coordinates from cty.dat alone.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_CALLSIGN_DISTRICTS_H
#define PIC_CALLSIGN_DISTRICTS_H

/*
 * pic_district_t - A single district prefix → location mapping.
 */
typedef struct {
    const char *prefix;  /* Callsign prefix (e.g., "W1", "VK6") */
    double lat;          /* Latitude (north positive)            */
    double lon;          /* Longitude (east positive)            */
} pic_district_t;

/*
 * pic_district_lookup - Look up district-level coordinates.
 *
 * Performs longest-prefix-match against the built-in district table.
 * Returns 1 and fills out_lat/out_lon if a match is found.
 * Returns 0 if no district match (caller should use cty.dat coords).
 */
int pic_district_lookup(const char *callsign, double *out_lat, double *out_lon);

#endif /* PIC_CALLSIGN_DISTRICTS_H */
