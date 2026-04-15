/*
 * tzlookup.c - Timezone lookup from geographic coordinates
 *
 * Uses a precomputed 0.5-degree grid (720x360 cells) mapping each
 * cell to an IANA timezone name. The grid file is generated at
 * build time by generate_tzgrid.py using the timezonefinder library.
 *
 * File format (TZGRID1):
 *   - Header: "TZGRID1\0" (8 bytes)
 *   - Grid: 720 * 360 = 259200 uint16_t values (518400 bytes)
 *     Each value is an index into the string table (0 = ocean/unknown)
 *   - String table: null-terminated timezone names, concatenated
 *     Index 0 = empty string (unknown), 1+ = timezone names
 *
 * Grid layout: row-major, starting at (-90, -180).
 *   cell[lat_idx * 720 + lon_idx]
 *   lat_idx = (int)((lat + 90) * 2)    (0 = -90°, 359 = +89.5°)
 *   lon_idx = (int)((lon + 180) * 2)   (0 = -180°, 719 = +179.5°)
 *
 * The 0.5-degree resolution (~55 km at equator) resolves most timezone
 * boundaries accurately. The full grid with ~424 timezone strings
 * totals about 513 KB in memory.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tzlookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GRID_W 720
#define GRID_H 360
#define GRID_CELLS (GRID_W * GRID_H)
#define HEADER_MAGIC "TZGRID1\0"
#define HEADER_LEN 8

static uint16_t *tz_grid = NULL;
static char *tz_strings = NULL;
static int tz_string_count = 0;

/* Offsets into the concatenated string table */
static const char **tz_names = NULL;

/*
 * pic_tz_load - Load the timezone grid from a binary file.
 *
 * Reads the grid and string table into memory for fast lookups.
 * The file is read once at startup; ~513 KB resident after load.
 *
 * Returns 0 on success, -1 on failure. On failure, pic_tz_lookup()
 * will return NULL (the renderer falls back to UTC offset display).
 */
int pic_tz_load(const char *path)
{
    FILE *f;
    long file_size, str_size;
    int i, count;
    char header[HEADER_LEN];
    char *p;

    f = fopen(path, "rb");
    if (!f) {
        printf("tzlookup: cannot open %s\n", path);
        return -1;
    }

    /* Verify magic header to catch wrong/corrupted files */
    if (fread(header, 1, HEADER_LEN, f) != HEADER_LEN ||
        memcmp(header, HEADER_MAGIC, HEADER_LEN) != 0) {
        fprintf(stderr, "tzlookup: invalid header in %s\n", path);
        fclose(f);
        return -1;
    }

    /* Read grid */
    tz_grid = malloc(GRID_CELLS * sizeof(uint16_t));
    if (!tz_grid) {
        fclose(f);
        return -1;
    }
    if (fread(tz_grid, sizeof(uint16_t), GRID_CELLS, f) != GRID_CELLS) {
        fprintf(stderr, "tzlookup: short read on grid data\n");
        free(tz_grid);
        tz_grid = NULL;
        fclose(f);
        return -1;
    }

    /* Read the rest as the string table */
    {
        long pos = ftell(f);
        fseek(f, 0, SEEK_END);
        file_size = ftell(f);
        str_size = file_size - pos;
        fseek(f, pos, SEEK_SET);
    }

    if (str_size <= 0) {
        fprintf(stderr, "tzlookup: no string table\n");
        free(tz_grid);
        tz_grid = NULL;
        fclose(f);
        return -1;
    }

    tz_strings = malloc(str_size);
    if (!tz_strings) {
        free(tz_grid);
        tz_grid = NULL;
        fclose(f);
        return -1;
    }
    if (fread(tz_strings, 1, str_size, f) != (size_t)str_size) {
        fprintf(stderr, "tzlookup: short read on string table\n");
        free(tz_grid);
        free(tz_strings);
        tz_grid = NULL;
        tz_strings = NULL;
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Count strings by counting NUL terminators, then build a pointer
     * index so grid uint16 values can be resolved to strings in O(1). */
    count = 0;
    for (i = 0; i < str_size; i++) {
        if (tz_strings[i] == '\0') count++;
    }

    tz_names = malloc(count * sizeof(char *));
    if (!tz_names) {
        free(tz_grid);
        free(tz_strings);
        tz_grid = NULL;
        tz_strings = NULL;
        return -1;
    }

    /* Walk the concatenated strings, recording the start of each one */
    p = tz_strings;
    tz_string_count = 0;
    for (i = 0; i < count && p < tz_strings + str_size; i++) {
        tz_names[tz_string_count++] = p;
        p += strlen(p) + 1;
    }

    printf("tzlookup: loaded %d timezone names, %d grid cells\n",
           tz_string_count, GRID_CELLS);
    return 0;
}

/*
 * pic_tz_lookup - Look up IANA timezone name for a lat/lon point.
 *
 * Converts coordinates to grid cell indices (0.5-degree resolution)
 * and returns the timezone name, or NULL for ocean/unknown cells.
 *
 * Coordinate mapping:
 *   lat_idx = (lat + 90) * 2     →  0..359  (south to north)
 *   lon_idx = (lon + 180) * 2    →  0..719  (west to east)
 */
const char *pic_tz_lookup(double lat, double lon)
{
    int lat_idx, lon_idx, cell;
    uint16_t tz_id;

    if (!tz_grid || !tz_names) return NULL;

    /* Clamp to valid range, avoiding exactly +90/+180 (out of grid) */
    if (lat < -90) lat = -90;
    if (lat > 89.99) lat = 89.99;
    if (lon < -180) lon = -180;
    if (lon > 179.99) lon = 179.99;

    lat_idx = (int)((lat + 90.0) * 2.0);
    lon_idx = (int)((lon + 180.0) * 2.0);

    if (lat_idx < 0) lat_idx = 0;
    if (lat_idx >= GRID_H) lat_idx = GRID_H - 1;
    if (lon_idx < 0) lon_idx = 0;
    if (lon_idx >= GRID_W) lon_idx = GRID_W - 1;

    cell = lat_idx * GRID_W + lon_idx;
    tz_id = tz_grid[cell];

    if (tz_id == 0 || tz_id >= tz_string_count) return NULL;

    return tz_names[tz_id];
}
