/*
 * lightning.h - Real-time lightning strike data from Blitzortung
 *
 * Connects to the Blitzortung community lightning network via a
 * raw TCP connection, receives JSON-encoded strike events, and
 * stores them in a circular buffer for the lightning map layer.
 *
 * Each strike has a lat/lon position and a timestamp. Strikes
 * are displayed briefly on the map (configurable fade time,
 * default 3 seconds) with a colour fade: white -> yellow -> red.
 *
 * Data source: Blitzortung.org (community volunteer sensor network)
 * Protocol: TCP with JSON frames, no authentication required
 * Coverage: Global (densest in Europe and North America)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_LIGHTNING_H
#define PIC_LIGHTNING_H

#include <pthread.h>
#include <time.h>
#include <sys/time.h>

/*
 * Maximum number of strikes held in the circular buffer.
 * At peak global activity Blitzortung delivers a few hundred
 * strikes per minute. With a 3-second display window we only
 * need to hold a few hundred at most. 512 is generous.
 */
#define PIC_MAX_STRIKES 512

/*
 * Default fade time in milliseconds — how long a strike stays
 * visible on the map before fully fading out.
 */
#define PIC_LIGHTNING_FADE_DEFAULT_MS 15000

/*
 * pic_strike_t - A single lightning strike event.
 */
typedef struct {
    double lat;             /* Strike latitude (degrees)              */
    double lon;             /* Strike longitude (degrees)             */
    struct timeval when;    /* When the strike was received (local)   */
} pic_strike_t;

/*
 * pic_lightning_t - Shared lightning strike buffer.
 *
 * Thread-safe circular buffer. The background fetcher thread
 * writes strikes; the renderer reads them each frame.
 */
typedef struct {
    pic_strike_t strikes[PIC_MAX_STRIKES];  /* Circular buffer      */
    int          head;                       /* Next write position  */
    int          count;                      /* Total strikes stored */
    int          fade_ms;                    /* Display fade time    */
    pthread_mutex_t mutex;
} pic_lightning_t;

/*
 * pic_lightning_init - Initialise the strike buffer.
 */
void pic_lightning_init(pic_lightning_t *data);

/*
 * pic_lightning_start - Start the Blitzortung background thread.
 *
 * Connects to the Blitzortung server and begins receiving strikes.
 * Reconnects automatically on disconnect with exponential backoff.
 *
 * Returns 0 on success (thread started), -1 on failure.
 */
int pic_lightning_start(pic_lightning_t *data);

/*
 * pic_lightning_stop - Stop the background thread.
 */
void pic_lightning_stop(void);

/*
 * pic_lightning_reload - Re-read fade time from config file.
 *
 * Called on config reload. Reads LIGHTNING_FADE_MS from
 * /data/etc/pi-clock-renderer.conf.
 */
void pic_lightning_reload(pic_lightning_t *data);

#endif /* PIC_LIGHTNING_H */
