/*
 * aurora.h - Aurora oval data from NOAA SWPC OVATION model
 *
 * Fetches the aurora nowcast directly from NOAA Space Weather
 * Prediction Center every 15 minutes. The data is an ASCII grid
 * of aurora probability values (0-100) on a 1-degree global grid.
 *
 * Data source: NOAA SWPC aurora-nowcast-map.txt
 * Free, no authentication required.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_AURORA_H
#define PIC_AURORA_H

#include <pthread.h>
#include <time.h>

/* Aurora grid dimensions — 1-degree global grid */
#define PIC_AURORA_NX 360
#define PIC_AURORA_NY 181

/*
 * pic_aurora_t - Aurora probability grid.
 *
 * Thread-safe: fetcher writes, renderer reads.
 */
typedef struct {
    float prob[PIC_AURORA_NY][PIC_AURORA_NX]; /* Probability 0-100 */
    time_t last_fetched;                       /* Last fetch time   */
    int    valid;                               /* 1 if data loaded  */
    pthread_mutex_t mutex;
} pic_aurora_t;

/*
 * pic_aurora_init - Initialise the aurora data buffer.
 */
void pic_aurora_init(pic_aurora_t *data);

/*
 * pic_aurora_start - Start the background fetch thread.
 *
 * Fetches immediately, then every 15 minutes.
 * Returns 0 on success, -1 on failure.
 */
int pic_aurora_start(pic_aurora_t *data);

/*
 * pic_aurora_stop - Stop the background fetch thread.
 */
void pic_aurora_stop(void);

#endif /* PIC_AURORA_H */
