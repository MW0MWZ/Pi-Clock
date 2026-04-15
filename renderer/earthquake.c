/*
 * earthquake.c - USGS earthquake data fetcher
 *
 * Runs a background thread that polls the USGS Earthquake Hazards
 * Program GeoJSON feed for significant earthquakes (M4.5+) in the
 * past 24 hours. Events are parsed and stored in a shared buffer
 * for the earthquake map layer to render.
 *
 * USGS GeoJSON format (simplified):
 *   {
 *     "features": [
 *       {
 *         "properties": {
 *           "mag": 5.2,
 *           "place": "15km NE of Reykjavik",
 *           "time": 1718000000000     <- Unix ms
 *         },
 *         "geometry": {
 *           "coordinates": [lon, lat, depth_km]
 *         }
 *       }, ...
 *     ]
 *   }
 *
 * We use simple string scanning rather than a JSON library — the
 * USGS format is stable and we only need a few fields.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "earthquake.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/*
 * USGS pre-built feed URL.
 * M4.5+ past day: small payload, CDN-served, updated every 5 min.
 */
#define USGS_FEED_URL \
    "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/4.5_day.geojson"

/* Fetch interval in seconds */
#define FETCH_INTERVAL 60

/* Maximum response size (the M4.5 daily feed is typically <100KB) */
#define MAX_RESPONSE (256 * 1024)

/* Thread state */
static pthread_t quake_thread;
static volatile int quake_running = 0;
static pic_earthquake_t *cfg_data = NULL;

/*
 * pic_earthquake_init - Initialise the earthquake buffer.
 */
void pic_earthquake_init(pic_earthquake_t *data)
{
    memset(data, 0, sizeof(*data));
    data->display_time_s = PIC_QUAKE_DISPLAY_DEFAULT_S;
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * parse_features - Parse the USGS GeoJSON features array.
 *
 * Scans through the response buffer looking for feature objects.
 * For each feature, extracts: coordinates (lon, lat, depth),
 * magnitude, origin time, and place description.
 *
 * This is a simple forward-scan parser — not a full JSON parser.
 * It relies on the USGS feed having a consistent structure where
 * each feature contains "coordinates", "mag", "time", and "place"
 * fields. This has been stable for over a decade.
 */
static int parse_features(const char *json, pic_quake_t *quakes, int max)
{
    const char *p = json;
    int count = 0;

    /* Find the "features" array */
    p = strstr(p, "\"features\"");
    if (!p) return 0;

    /* Scan for each feature object */
    while (count < max && (p = strstr(p, "\"geometry\"")) != NULL) {
        pic_quake_t q;
        const char *coords;
        const char *props;

        memset(&q, 0, sizeof(q));

        /* Extract coordinates: [lon, lat, depth] */
        coords = strstr(p, "\"coordinates\"");
        if (!coords) break;
        coords = strchr(coords, '[');
        if (!coords) break;
        coords++;

        {
            char *end;
            q.lon = strtod(coords, &end);
            if (*end != ',') { p = coords; continue; }
            q.lat = strtod(end + 1, &end);
            if (*end != ',') { p = coords; continue; }
            q.depth_km = strtod(end + 1, NULL);
        }

        /* Sanity check coordinates */
        if (q.lat < -90 || q.lat > 90 ||
            q.lon < -180 || q.lon > 180) {
            p = coords;
            continue;
        }

        /* Look backwards for properties (they come before geometry) */
        {
            /*
             * Properties appear before geometry in each feature.
             * Search backwards from current position to find "mag".
             * Since we're scanning forward, we can also look ahead
             * for properties that appear after geometry in some
             * variants. Try both directions.
             */
            const char *mag_p = NULL;
            const char *search;

            /* Search backwards up to 2000 chars for "mag".
             * Compute safe lower bound to avoid UB from pointer
             * arithmetic before the start of the buffer. */
            {
                const char *lower = (coords - json > 2000) ? coords - 2000 : json;
            for (search = coords - 1;
                 search >= lower;
                 search--) {
                if (strncmp(search, "\"mag\":", 6) == 0) {
                    mag_p = search;
                    break;
                }
            }
            } /* close lower-bound scope */

            /* Try forward if not found */
            if (!mag_p) {
                mag_p = strstr(coords, "\"mag\":");
                if (!mag_p || mag_p > coords + 2000) {
                    p = coords + 1;
                    continue;
                }
            }

            q.magnitude = strtod(mag_p + 6, NULL);
        }

        /* Extract origin time (Unix milliseconds) */
        {
            const char *time_p = NULL;
            const char *search;

            {
                const char *lower = (coords - json > 2000) ? coords - 2000 : json;
            for (search = coords - 1;
                 search >= lower;
                 search--) {
                if (strncmp(search, "\"time\":", 7) == 0) {
                    time_p = search;
                    break;
                }
            }
            }

            if (time_p) {
                long long ms = strtoll(time_p + 7, NULL, 10);
                q.origin_time = (time_t)(ms / 1000);
            } else {
                q.origin_time = time(NULL);
            }
        }

        /* Extract place description */
        {
            const char *place_p = NULL;
            const char *search;

            {
                const char *lower = (coords - json > 2000) ? coords - 2000 : json;
            for (search = coords - 1;
                 search >= lower;
                 search--) {
                if (strncmp(search, "\"place\":\"", 9) == 0) {
                    place_p = search + 9;
                    break;
                }
            }
            }

            if (place_p) {
                const char *end = strchr(place_p, '"');
                if (end) {
                    int len = end - place_p;
                    if (len > (int)sizeof(q.place) - 1)
                        len = sizeof(q.place) - 1;
                    memcpy(q.place, place_p, len);
                    q.place[len] = '\0';
                }
            }
        }

        q.active = 1;
        quakes[count++] = q;

        /* Advance past this feature */
        p = coords + 1;
    }

    return count;
}

/*
 * quake_thread_func - Background thread for polling USGS.
 *
 * Fetches the GeoJSON feed, parses features, and replaces the
 * shared buffer contents. The full buffer is replaced on each
 * fetch rather than incrementally updated — this is simpler and
 * the USGS feed is small enough that it's not worth the
 * complexity of differential updates.
 */
static void *quake_thread_func(void *arg)
{
    char *buf;

    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) {
        fprintf(stderr, "earthquake: failed to allocate fetch buffer\n");
        return NULL;
    }

    /* Wait for network connectivity */
    if (!pic_wait_for_network("earthquake", &quake_running)) {
        free(buf);
        return NULL;
    }

    while (quake_running) {
        int n;

        n = pic_fetch_to_buf(USGS_FEED_URL, buf, MAX_RESPONSE - 1);
        if (n > 0) {
            buf[n] = '\0'; /* Belt-and-suspenders null termination */
            pic_quake_t temp[PIC_MAX_QUAKES];
            int count;

            count = parse_features(buf, temp, PIC_MAX_QUAKES);

            pthread_mutex_lock(&cfg_data->mutex);
            memcpy(cfg_data->quakes, temp,
                   count * sizeof(pic_quake_t));
            cfg_data->count = count;
            cfg_data->last_fetched = time(NULL);
            pthread_mutex_unlock(&cfg_data->mutex);

            printf("earthquake: fetched %d events from USGS\n", count);
        } else {
            printf("earthquake: fetch failed, retrying in %ds\n",
                   FETCH_INTERVAL);
        }

        /* Sleep in 1-second chunks so the stop flag is checked promptly */
        {
            int s;
            for (s = 0; s < FETCH_INTERVAL && quake_running; s++)
                sleep(1);
        }
    }

    free(buf);
    return NULL;
}

/*
 * pic_earthquake_start - Start the USGS fetch thread.
 */
int pic_earthquake_start(pic_earthquake_t *data)
{
    if (quake_running) return 0;

    cfg_data = data;

    /* Read display time from config */
    pic_earthquake_reload(data);

    quake_running = 1;

    if (pthread_create(&quake_thread, NULL,
                       quake_thread_func, NULL) != 0) {
        quake_running = 0;
        fprintf(stderr, "earthquake: failed to create thread\n");
        return -1;
    }

    printf("earthquake: fetcher thread started (display=%ds)\n",
           data->display_time_s);
    return 0;
}

/*
 * pic_earthquake_stop - Stop the background fetch thread.
 */
void pic_earthquake_stop(void)
{
    if (!quake_running) return;
    quake_running = 0;
    pthread_join(quake_thread, NULL);
    cfg_data = NULL;
    printf("earthquake: fetcher thread stopped\n");
}

/*
 * pic_earthquake_reload - Re-read display time from config.
 *
 * Reads EARTHQUAKE_DISPLAY_S from the renderer config file.
 * Clamped to 60-86400 seconds (1 minute to 24 hours).
 * Default is 86400 (24 hours) to match the USGS daily feed.
 */
void pic_earthquake_reload(pic_earthquake_t *data)
{
    FILE *f;
    char line[256];
    int new_time = data->display_time_s;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "EARTHQUAKE_DISPLAY_S=", 21) == 0) {
                new_time = atoi(line + 21);
            }
        }
        fclose(f);
    }

    /* Clamp to sane range: 1 minute to 2 hours */
    if (new_time < 60) new_time = 60;
    if (new_time > 86400) new_time = 86400;

    pthread_mutex_lock(&data->mutex);
    data->display_time_s = new_time;
    pthread_mutex_unlock(&data->mutex);

    printf("earthquake: display time = %ds\n", new_time);
}
