/*
 * ctydat.h - Country/prefix database (cty.dat) for callsign lookup
 *
 * Parses the AD1C BigCTY database to map amateur radio callsign
 * prefixes to countries with geographic coordinates, CQ zones,
 * ITU zones, and continent codes.
 *
 * The lookup uses a longest-prefix-match algorithm: given a
 * callsign like "VK2ABC", it tries "VK2ABC", "VK2AB", "VK2A",
 * "VK2", "VK", "V" until it finds a match in the prefix table.
 *
 * IMPORTANT: cty.dat uses West-positive longitude convention.
 * All longitudes returned by this module are converted to the
 * standard East-positive convention used by the renderer.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_CTYDAT_H
#define PIC_CTYDAT_H

/*
 * pic_country_t - Information about a DXCC entity / country.
 */
typedef struct {
    char name[64];        /* Country name                        */
    char prefix[16];      /* Primary prefix (e.g., "K", "G")     */
    char continent[4];    /* Continent code (NA/SA/EU/AF/AS/OC)  */
    int  cq_zone;         /* CQ zone number (1-40)               */
    int  itu_zone;        /* ITU zone number                     */
    double lat;           /* Latitude (north positive)           */
    double lon;           /* Longitude (east positive)           */
    double utc_offset;    /* Hours from UTC                      */
} pic_country_t;

/*
 * pic_ctydat_t - The loaded prefix database.
 *
 * Contains a hash table of prefix → country mappings built
 * from the cty.dat file. Use pic_ctydat_lookup() to query.
 */
typedef struct pic_ctydat pic_ctydat_t;

/*
 * pic_ctydat_load - Load and parse a cty.dat file.
 *
 * Returns a pointer to the database, or NULL on failure.
 * The caller must free it with pic_ctydat_free().
 */
pic_ctydat_t *pic_ctydat_load(const char *path);

/*
 * pic_ctydat_lookup - Look up a callsign in the database.
 *
 * Uses longest-prefix-match to find the country for a callsign.
 * Returns a pointer to the country info, or NULL if not found.
 * The returned pointer is valid until pic_ctydat_free() is called.
 */
const pic_country_t *pic_ctydat_lookup(const pic_ctydat_t *db,
                                       const char *callsign);

/*
 * pic_ctydat_lookup_location - Look up callsign with best-available coordinates.
 *
 * Returns the country info AND the most precise lat/lon available.
 * If BigCTY provides a per-prefix <lat/lon> override (e.g., US callsign
 * districts, Canadian provinces), those coordinates are returned instead
 * of the parent entity centre. This places DX spots more accurately
 * within large countries.
 */
const pic_country_t *pic_ctydat_lookup_location(const pic_ctydat_t *db,
                                                 const char *callsign,
                                                 double *out_lat,
                                                 double *out_lon);

/*
 * pic_ctydat_free - Free the database.
 */
void pic_ctydat_free(pic_ctydat_t *db);

#endif /* PIC_CTYDAT_H */
