/*
 * satellite.h - Satellite tracking via TLE + SGP4
 *
 * Downloads TLE data from Celestrak, propagates orbits using a
 * simplified SGP4 model, and provides current satellite positions
 * for rendering on the map.
 *
 * Supported satellites are configurable — defaults include the ISS
 * and common amateur radio satellites.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_SATELLITE_H
#define PIC_SATELLITE_H

#include <time.h>
#include <pthread.h>

#define SAT_MAX 32
#define SAT_NAME_LEN 32
#define SAT_TRACK_PTS 120   /* Ground track points (past + future) */

/*
 * pic_sat_t - A tracked satellite.
 */
typedef struct {
    char name[SAT_NAME_LEN];  /* Common name (e.g. "ISS")           */
    char config_key[SAT_NAME_LEN]; /* Config key (e.g. "iss")       */
    int  norad_id;            /* NORAD catalog number                */
    int  enabled;             /* Non-zero if tracking this satellite */

    /* TLE orbital elements (parsed from two-line element set) */
    double epoch_jd;          /* Epoch as Julian date                */
    double inclination;       /* Inclination (radians)               */
    double raan;              /* Right ascension of ascending node   */
    double eccentricity;      /* Eccentricity                        */
    double arg_perigee;       /* Argument of perigee (radians)       */
    double mean_anomaly;      /* Mean anomaly at epoch (radians)     */
    double mean_motion;       /* Mean motion (revs/day)              */
    double bstar;             /* B* drag term                        */
    int    tle_loaded;        /* Non-zero if TLE data is valid       */

    /* Current computed position */
    double lat;               /* Sub-satellite latitude (degrees)    */
    double lon;               /* Sub-satellite longitude (degrees)   */
    double alt_km;            /* Altitude above Earth (km)           */
    double footprint_r;       /* Footprint radius (degrees)          */

    /* Ground track: past 25% + future 75% of one orbit */
    double track_lat[SAT_TRACK_PTS];
    double track_lon[SAT_TRACK_PTS];
    int    track_now_idx;     /* Index of "now" in the track array   */
    int    track_count;       /* Number of valid track points        */
} pic_sat_t;

/*
 * pic_satlist_t - List of tracked satellites.
 */
typedef struct {
    pic_sat_t sats[SAT_MAX];
    int count;
    time_t last_tle_update;
    pthread_mutex_t mutex;
} pic_satlist_t;

void pic_sat_init(pic_satlist_t *list);

/*
 * pic_sat_add - Register a satellite to track.
 */
int pic_sat_add(pic_satlist_t *list, const char *config_key,
                const char *name, int norad_id);

/*
 * pic_sat_start - Start the background TLE fetch + propagation thread.
 */
int pic_sat_start(pic_satlist_t *list);
void pic_sat_stop(void);

/*
 * pic_sat_propagate - Update all satellite positions for the given time.
 * Called from the render loop.
 */
void pic_sat_propagate(pic_satlist_t *list, time_t now);

/*
 * pic_sat_load_config - Load satellite enable/disable from config.
 * File: /data/etc/pi-clock-satellites.conf, format: name=0|1
 */
void pic_sat_load_config(pic_satlist_t *list);

#endif /* PIC_SATELLITE_H */
