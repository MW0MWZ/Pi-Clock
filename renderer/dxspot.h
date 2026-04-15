/*
 * dxspot.h - DX spot data structures and management
 *
 * Holds a list of recent DX cluster spots with their geographic
 * coordinates (resolved from callsign prefixes via cty.dat).
 * Thread-safe for concurrent access from the telnet client
 * thread and the render thread.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_DXSPOT_H
#define PIC_DXSPOT_H

#include <time.h>
#include <pthread.h>

/* Maximum number of simultaneous spots on the display */
#define MAX_SPOTS 100

/* Number of recent spot lines to keep for the live feed display */
#define FEED_LINES 12
#define FEED_LINE_LEN 80

/* Spot aging: configurable, default 15 minutes */
extern int pic_spot_max_age;

/*
 * pic_dxspot_t - A single DX spot.
 */
typedef struct {
    char spotter[16];       /* Spotter callsign                  */
    char dx_call[16];       /* DX station callsign               */
    double freq_khz;        /* Frequency in kHz                  */
    char comment[32];       /* Free-form comment                 */
    time_t timestamp;       /* When the spot was received (UTC)  */

    /* Resolved geographic coordinates */
    double spotter_lat;     /* Spotter latitude                  */
    double spotter_lon;     /* Spotter longitude                 */
    double dx_lat;          /* DX station latitude               */
    double dx_lon;          /* DX station longitude              */

    /* Derived */
    int band_index;         /* Index into band colour table      */
    int is_home_country;    /* Spotter is in user's home country */
    int active;             /* Non-zero if this slot is in use   */
} pic_dxspot_t;

/*
 * pic_spotlist_t - Thread-safe list of active DX spots.
 */
typedef struct {
    pic_dxspot_t spots[MAX_SPOTS];
    int count;

    /* Circular buffer of recent spot lines for the live feed display */
    char feed[FEED_LINES][FEED_LINE_LEN];
    int feed_head;  /* Next write position */
    int feed_count; /* Number of lines in the buffer */

    /* Active band mask — bit N = band index N enabled.
     * Updated by dxcluster.c when config changes. Read by
     * the DX spots layer to dim disabled bands in the legend. */
    unsigned int band_mask;

    /* Pointer to ticker for positioning the band legend above it */
    void *ticker;

    pthread_mutex_t mutex;
} pic_spotlist_t;

/*
 * pic_spotlist_init - Initialise the spot list.
 */
void pic_spotlist_init(pic_spotlist_t *list);

/*
 * pic_spotlist_add - Add a new spot to the list.
 *
 * If the list is full, the oldest spot is replaced.
 * Deduplicates: if the same DX callsign on the same band
 * already exists, the existing spot is updated instead.
 */
void pic_spotlist_add(pic_spotlist_t *list, const pic_dxspot_t *spot);

/*
 * pic_spotlist_filter_bands - Remove spots on disabled bands.
 *
 * Called when the band mask changes to immediately clear
 * spots that are no longer wanted.
 */
void pic_spotlist_filter_bands(pic_spotlist_t *list, unsigned int band_mask);

/*
 * pic_spotlist_feed_push - Add a line to the live feed buffer.
 */
void pic_spotlist_feed_push(pic_spotlist_t *list, const char *line);

/*
 * pic_spotlist_expire - Remove spots older than SPOT_MAX_AGE.
 */
void pic_spotlist_expire(pic_spotlist_t *list, time_t now);

/*
 * pic_freq_to_band - Convert frequency in kHz to a band index.
 *
 * Returns 0-11 for 160m through 2m, or -1 for unknown.
 */
int pic_freq_to_band(double freq_khz);

/*
 * pic_band_name - Get the display name for a band index.
 * Returns e.g. "160m", "80m", ... "2m", or "?" for unknown.
 */
const char *pic_band_name(int band_index);

/*
 * pic_band_count - Return the number of defined bands.
 */
int pic_band_count(void);

/*
 * pic_band_colour - Get RGBA colour for a band index.
 *
 * Fills r, g, b, a with values 0.0-1.0.
 */
void pic_band_colour(int band_index, double *r, double *g, double *b, double *a);

#endif /* PIC_DXSPOT_H */
