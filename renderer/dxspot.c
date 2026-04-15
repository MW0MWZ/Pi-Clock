/*
 * dxspot.c - DX spot list management and band definitions
 *
 * Thread-safe fixed-size ring of active DX spots plus a circular
 * live-feed buffer for the scrolling panel display.
 *
 * Design choices for the Pi Zero W:
 *   - Fixed-size array (MAX_SPOTS=100) avoids malloc during runtime.
 *   - Dedup by (dx_call, band) prevents the same station filling
 *     all slots when a cluster floods repeated spots.
 *   - When full, the oldest spot is evicted (LRU policy).
 *   - Band definitions are the single source of truth: all colour
 *     and name lookups go through pic_band_colour/pic_band_name.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "dxspot.h"
#include <string.h>
#include <stdio.h>

/* Default spot max age — configurable via DX_SPOT_AGE in config */
int pic_spot_max_age = 900;

/* Band frequency ranges (kHz) and their display colours */
static const struct {
    double low, high;
    const char *name;
    double r, g, b;     /* RGB 0.0-1.0 */
} bands[] = {
    {  1800,   2000, "160m", 0.55, 0.00, 0.00 },  /* Dark red    */
    {  3500,   4000,  "80m", 1.00, 0.27, 0.00 },  /* Orange-red  */
    {  5330,   5410,  "60m", 1.00, 0.55, 0.00 },  /* Dark orange */
    {  7000,   7300,  "40m", 1.00, 0.65, 0.00 },  /* Orange      */
    { 10100,  10150,  "30m", 1.00, 1.00, 0.00 },  /* Yellow      */
    { 14000,  14350,  "20m", 0.00, 1.00, 0.00 },  /* Green       */
    { 18068,  18168,  "17m", 0.00, 1.00, 1.00 },  /* Cyan        */
    { 21000,  21450,  "15m", 0.30, 0.50, 1.00 },  /* Blue        */
    { 24890,  24990,  "12m", 0.54, 0.17, 0.89 },  /* Violet      */
    { 28000,  29700,  "10m", 1.00, 0.00, 1.00 },  /* Magenta     */
    { 50000,  54000,   "6m", 1.00, 1.00, 1.00 },  /* White       */
    {144000, 148000,   "2m", 0.67, 0.67, 0.67 },  /* Grey        */
};
#define NUM_BANDS (sizeof(bands) / sizeof(bands[0]))

/* Initialise spot list: zero all fields, enable all bands, init mutex. */
void pic_spotlist_init(pic_spotlist_t *list)
{
    memset(list, 0, sizeof(*list));
    list->band_mask = 0xFFFF;  /* All bands enabled by default */
    pthread_mutex_init(&list->mutex, NULL);
}

/* Map a frequency in kHz to a band index (0-11) or -1 if unknown.
 * Scans the bands[] table for a range match. */
int pic_freq_to_band(double freq_khz)
{
    int i;
    for (i = 0; i < (int)NUM_BANDS; i++) {
        if (freq_khz >= bands[i].low && freq_khz <= bands[i].high) {
            return i;
        }
    }
    return -1;
}

const char *pic_band_name(int band_index)
{
    if (band_index >= 0 && band_index < (int)NUM_BANDS)
        return bands[band_index].name;
    return "?";
}

int pic_band_count(void)
{
    return (int)NUM_BANDS;
}

void pic_band_colour(int band_index, double *r, double *g, double *b, double *a)
{
    if (band_index >= 0 && band_index < (int)NUM_BANDS) {
        *r = bands[band_index].r;
        *g = bands[band_index].g;
        *b = bands[band_index].b;
        *a = 0.8;
    } else {
        *r = *g = *b = 0.5;
        *a = 0.5;
    }
}

/*
 * pic_spotlist_add - Insert or update a spot in the list.
 *
 * Dedup: if an active spot with the same dx_call and band exists,
 * it is updated in place (timestamp refreshed). Otherwise, the spot
 * goes into the first empty slot or replaces the oldest spot.
 * Thread-safe: acquires the list mutex.
 */
void pic_spotlist_add(pic_spotlist_t *list, const pic_dxspot_t *spot)
{
    int i, oldest_idx = 0;
    time_t oldest_time;

    pthread_mutex_lock(&list->mutex);

    /* Check for duplicate: same DX callsign on same band */
    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active &&
            list->spots[i].band_index == spot->band_index &&
            strcmp(list->spots[i].dx_call, spot->dx_call) == 0) {
            /* Update existing spot (refresh timestamp) */
            list->spots[i] = *spot;
            pthread_mutex_unlock(&list->mutex);
            return;
        }
    }

    /* Find an empty slot or the oldest spot */
    /* No standard TIME_MAX constant. time_t is typically signed long,
     * so max value = (unsigned long)(-1) >> 1. This ensures any active
     * spot's timestamp will be less than oldest_time. */
    oldest_time = (time_t)((unsigned long)-1 >> 1);
    for (i = 0; i < MAX_SPOTS; i++) {
        if (!list->spots[i].active) {
            oldest_idx = i;
            break;
        }
        if (list->spots[i].timestamp < oldest_time) {
            oldest_time = list->spots[i].timestamp;
            oldest_idx = i;
        }
    }

    list->spots[oldest_idx] = *spot;

    /* Recount active spots */
    list->count = 0;
    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active) list->count++;
    }

    pthread_mutex_unlock(&list->mutex);
}

void pic_spotlist_filter_bands(pic_spotlist_t *list, unsigned int bmask)
{
    int i;
    pthread_mutex_lock(&list->mutex);
    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active && list->spots[i].band_index >= 0 &&
            !(bmask & (1u << list->spots[i].band_index))) {
            list->spots[i].active = 0;
        }
    }
    list->count = 0;
    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active) list->count++;
    }
    pthread_mutex_unlock(&list->mutex);
}

void pic_spotlist_feed_push(pic_spotlist_t *list, const char *line)
{
    pthread_mutex_lock(&list->mutex);
    strncpy(list->feed[list->feed_head], line, FEED_LINE_LEN - 1);
    list->feed[list->feed_head][FEED_LINE_LEN - 1] = '\0';
    list->feed_head = (list->feed_head + 1) % FEED_LINES;
    if (list->feed_count < FEED_LINES) list->feed_count++;
    pthread_mutex_unlock(&list->mutex);
}

/* Remove spots older than pic_spot_max_age seconds. Thread-safe. */
void pic_spotlist_expire(pic_spotlist_t *list, time_t now)
{
    int i;

    pthread_mutex_lock(&list->mutex);

    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active &&
            (now - list->spots[i].timestamp) > pic_spot_max_age) {
            list->spots[i].active = 0;
        }
    }

    /* Recount */
    list->count = 0;
    for (i = 0; i < MAX_SPOTS; i++) {
        if (list->spots[i].active) list->count++;
    }

    pthread_mutex_unlock(&list->mutex);
}
