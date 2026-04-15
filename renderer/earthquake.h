/*
 * earthquake.h - Real-time earthquake data from USGS
 *
 * Fetches significant earthquakes from the USGS Earthquake Hazards
 * Program GeoJSON feed. Events are stored with location, magnitude,
 * and timestamp for display as animated ripple markers on the map.
 *
 * Data source: USGS (earthquake.usgs.gov)
 * Format: GeoJSON — public domain, no authentication
 * Feed: M4.5+ past 24 hours (~20-80 KB per fetch)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_EARTHQUAKE_H
#define PIC_EARTHQUAKE_H

#include <pthread.h>
#include <time.h>

/*
 * Maximum number of earthquakes held in the buffer.
 * The USGS M4.5+ daily feed typically returns 15-60 events.
 * 128 is generous headroom.
 */
#define PIC_MAX_QUAKES 128

/*
 * Default display time in seconds — how long an earthquake
 * remains visible on the map before fading out.
 * Default: 24 hours (86400 seconds). The USGS feed provides
 * M4.5+ events from the past 24 hours, so matching the display
 * time to the feed window shows all available data.
 */
#define PIC_QUAKE_DISPLAY_DEFAULT_S 86400

/*
 * pic_quake_t - A single earthquake event.
 */
typedef struct {
    double lat;             /* Epicentre latitude (degrees)           */
    double lon;             /* Epicentre longitude (degrees)          */
    double magnitude;       /* Magnitude (e.g. 4.5, 6.2, 7.8)       */
    double depth_km;        /* Depth in kilometres                    */
    time_t origin_time;     /* When the earthquake occurred (UTC)     */
    char   place[64];       /* Human-readable location description    */
    int    active;          /* 1 if event should be displayed         */
} pic_quake_t;

/*
 * pic_earthquake_t - Shared earthquake event buffer.
 *
 * Thread-safe: the background fetcher writes events, the
 * renderer reads them each frame.
 */
typedef struct {
    pic_quake_t quakes[PIC_MAX_QUAKES]; /* Event buffer             */
    int         count;                   /* Number of active events  */
    int         display_time_s;          /* How long to show (secs)  */
    time_t      last_fetched;            /* Last successful fetch    */
    pthread_mutex_t mutex;
} pic_earthquake_t;

/*
 * pic_earthquake_init - Initialise the earthquake buffer.
 */
void pic_earthquake_init(pic_earthquake_t *data);

/*
 * pic_earthquake_start - Start the USGS background fetch thread.
 *
 * Fetches immediately, then every 60 seconds. The USGS feed is
 * updated every 5 minutes, so 60-second polling catches new
 * events promptly without excessive requests.
 *
 * Returns 0 on success (thread started), -1 on failure.
 */
int pic_earthquake_start(pic_earthquake_t *data);

/*
 * pic_earthquake_stop - Stop the background fetch thread.
 */
void pic_earthquake_stop(void);

/*
 * pic_earthquake_reload - Re-read display time from config file.
 *
 * Called on config reload. Reads EARTHQUAKE_DISPLAY_S from
 * /data/etc/pi-clock-renderer.conf.
 */
void pic_earthquake_reload(pic_earthquake_t *data);

#endif /* PIC_EARTHQUAKE_H */
