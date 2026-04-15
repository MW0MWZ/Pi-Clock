/*
 * solarweather.h - Solar weather data from NOAA SWPC
 *
 * Fetches solar indices in the background using the NOAA Space
 * Weather Prediction Center JSON API. Data is used by the solar
 * weather applet and feeds the real SFI into the MUF estimator.
 *
 * All endpoints are free, no authentication required.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_SOLARWEATHER_H
#define PIC_SOLARWEATHER_H

#include <cairo/cairo.h>
#include <time.h>
#include <pthread.h>

/*
 * pic_solar_data_t - Current solar weather conditions.
 *
 * All values are 0 until the first successful fetch.
 * Thread-safe: lock mutex before reading.
 */
typedef struct {
    /* Solar flux index (10.7 cm radio flux, SFU) */
    int sfi;

    /* Planetary K-index (0-9, geomagnetic disturbance) */
    double kp;

    /* Solar wind speed (km/s) */
    int solar_wind_speed;

    /* Interplanetary magnetic field */
    double bz;   /* Bz GSM component (nT, negative = southward) */
    int bt;       /* Total field magnitude (nT) */

    /* X-ray flare class (e.g. "B3.4", "M1.2", "X2.1") */
    char xray_class[8];

    /* SDO solar disc images — multiple wavelengths, cycled on display.
     * SDO_IMAGE_COUNT is defined here (inside the struct block) so the
     * constant and the arrays it sizes are co-located. The #define is
     * still file-scoped and usable by other translation units. */
    #define SDO_IMAGE_COUNT 5
    cairo_surface_t *sun_images[SDO_IMAGE_COUNT]; /* Loaded PNG images   */
    const char *sun_labels[SDO_IMAGE_COUNT];      /* Wavelength labels   */
    int sun_image_count;                          /* How many are loaded */

    /* Timestamps */
    time_t last_updated;  /* When data was last fetched */

    pthread_mutex_t mutex;
} pic_solar_data_t;

/*
 * pic_solar_init - Initialise the solar data structure.
 */
void pic_solar_init(pic_solar_data_t *data);

/*
 * pic_solar_start - Start the background fetch thread.
 *
 * Fetches data immediately, then every 15 minutes.
 */
int pic_solar_start(pic_solar_data_t *data);

/*
 * pic_solar_stop - Stop the background fetch thread.
 */
void pic_solar_stop(void);

#endif /* PIC_SOLARWEATHER_H */
