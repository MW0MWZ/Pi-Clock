/*
 * tzlookup.h - Timezone lookup from geographic coordinates
 *
 * Loads a precomputed 0.5-degree grid file (720x360 cells) that
 * maps each lat/lon cell to an IANA timezone name. Generated at
 * build time by generate_tzgrid.py using the timezonefinder library.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_TZLOOKUP_H
#define PIC_TZLOOKUP_H

/*
 * pic_tz_lookup - Look up timezone name from coordinates.
 *
 *   lat - Latitude in degrees (north positive)
 *   lon - Longitude in degrees (east positive)
 *
 * Returns an IANA timezone name (e.g. "America/Denver") or
 * NULL if no timezone data is available for that location.
 * The returned string is valid until the next call.
 */
const char *pic_tz_lookup(double lat, double lon);

/*
 * pic_tz_load - Load the timezone grid from a file.
 *
 * Call once at startup. Returns 0 on success, -1 on failure.
 * If the grid file is not available, pic_tz_lookup() will
 * return NULL and the renderer falls back to UTC offset.
 */
int pic_tz_load(const char *path);

#endif /* PIC_TZLOOKUP_H */
