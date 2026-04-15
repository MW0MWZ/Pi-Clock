/*
 * borders.h - Country border data loading and rendering
 *
 * Loads country border coordinates from a simple binary data file
 * generated at build time from Natural Earth's 110m admin-0
 * boundaries dataset.
 *
 * Binary format (.borders):
 *
 *   Header:
 *     uint32_t  num_polygons     - Total number of border polygons
 *
 *   For each polygon:
 *     uint32_t  num_points       - Number of coordinate pairs
 *     float     lon[num_points]  - Longitude values in degrees
 *     float     lat[num_points]  - Latitude values in degrees
 *
 * This format is compact (~200KB for 110m data), fast to parse
 * (no string processing), and trivial to generate from GeoJSON
 * with a small Python script.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_BORDERS_H
#define PIC_BORDERS_H

#include <stdint.h>

/*
 * pic_polygon_t - A single border polygon (coastline or boundary segment).
 *
 * Each polygon is a sequence of lon/lat coordinate pairs forming
 * a closed or open polyline. Countries with multiple disconnected
 * parts (e.g., islands) are represented as multiple polygons.
 */
typedef struct {
    uint32_t num_points;    /* Number of coordinate pairs          */
    float   *lons;          /* Array of longitude values (degrees) */
    float   *lats;          /* Array of latitude values (degrees)  */
} pic_polygon_t;

/*
 * pic_borders_t - Complete border dataset.
 *
 * Holds all polygons loaded from the binary data file.
 * The entire dataset is allocated as a single block for
 * cache-friendly access during rendering.
 */
typedef struct {
    uint32_t       num_polygons;   /* Total number of polygons  */
    pic_polygon_t *polygons;       /* Array of polygon structs  */
} pic_borders_t;

/*
 * pic_borders_load - Load border data from a binary file.
 *
 * Reads the .borders binary file and populates the borders struct.
 * The caller must free the data with pic_borders_free() when done.
 *
 *   path    - Path to the .borders binary file.
 *   borders - Pointer to struct to populate.
 *
 * Returns 0 on success, -1 on failure.
 */
int pic_borders_load(const char *path, pic_borders_t *borders);

/*
 * pic_borders_free - Free all memory allocated by pic_borders_load().
 */
void pic_borders_free(pic_borders_t *borders);

#endif /* PIC_BORDERS_H */
