/*
 * wind.h - Global surface wind data from NOAA GFS
 *
 * Fetches pre-processed wind data (U and V components at 10m above
 * ground) from the Pi-Clock data server. The data is produced by a
 * GitHub Action that runs every 6 hours, downloading from NOAA NOMADS
 * and converting to a compact binary format.
 *
 * Binary format (.wind.gz):
 *   4 bytes:  magic "WIND"
 *   4 bytes:  nx (uint32 LE) = 360
 *   4 bytes:  ny (uint32 LE) = 181
 *   8 bytes:  reference time (uint64 LE, unix timestamp)
 *   4 bytes:  reserved
 *   4 bytes:  reserved
 *   nx*ny*4:  U-component (float32 LE, m/s, row-major N->S)
 *   nx*ny*4:  V-component (float32 LE, m/s, row-major N->S)
 *
 * Grid: 1-degree resolution, 360 columns (0E to 359E) x 181 rows
 * (90N to 90S). Total ~509 KB uncompressed, ~270 KB gzipped.
 *
 * Data source: NOAA Global Forecast System (GFS), public domain.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_WIND_H
#define PIC_WIND_H

#include <pthread.h>
#include <time.h>

/* Wind grid dimensions — 1-degree global grid */
#define PIC_WIND_NX 360
#define PIC_WIND_NY 181

/*
 * pic_wind_t - Global wind data buffer.
 *
 * Thread-safe: the background fetcher writes the grid arrays,
 * the renderer reads them. Protected by mutex.
 */
typedef struct {
    float u[PIC_WIND_NY][PIC_WIND_NX]; /* Eastward wind (m/s)    */
    float v[PIC_WIND_NY][PIC_WIND_NX]; /* Northward wind (m/s)   */
    float cloud[PIC_WIND_NY][PIC_WIND_NX]; /* Cloud cover (0-100%)*/
    float precip[PIC_WIND_NY][PIC_WIND_NX];/* Precipitation (mm)  */
    int    has_cloud;                   /* 1 if cloud data loaded */
    int    has_precip;                  /* 1 if precip data loaded*/
    time_t ref_time;                    /* GFS cycle time         */
    time_t last_fetched;                /* When we last downloaded*/
    int    valid;                       /* 1 if wind data loaded  */
    pthread_mutex_t mutex;
} pic_wind_t;

/*
 * pic_wind_init - Initialise the wind data buffer.
 */
void pic_wind_init(pic_wind_t *data);

/*
 * pic_wind_start - Start the background fetch thread.
 *
 * Fetches immediately on start, then every 2 hours.
 * Returns 0 on success (thread started), -1 on failure.
 */
int pic_wind_start(pic_wind_t *data);

/*
 * pic_wind_stop - Stop the background fetch thread.
 */
void pic_wind_stop(void);

#endif /* PIC_WIND_H */
