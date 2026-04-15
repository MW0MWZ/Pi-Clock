/*
 * aurora.c - NOAA SWPC aurora nowcast fetcher
 *
 * Fetches the OVATION aurora nowcast ASCII grid from NOAA SWPC
 * every 15 minutes. The grid contains aurora probability (0-100)
 * at each 1-degree lat/lon point.
 *
 * The ASCII format is simple: lines of "lon lat probability"
 * values, with comment lines starting with #.
 *
 * Data source: https://services.swpc.noaa.gov/text/aurora-nowcast-map.txt
 * Free, no authentication, public domain US government data.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aurora.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* NOAA SWPC OVATION aurora nowcast endpoint (JSON with geographic coords) */
#define AURORA_URL \
    "https://services.swpc.noaa.gov/json/ovation_aurora_latest.json"

/* Fetch interval: 15 minutes (900 seconds) */
#define AURORA_FETCH_INTERVAL 900

/* Maximum response size — the JSON is typically ~2 MB */
#define MAX_RESPONSE (3 * 1024 * 1024)

/* Thread state */
static pthread_t aurora_thread;
static volatile int aurora_running = 0;
static pic_aurora_t *cfg_data = NULL;

/*
 * pic_aurora_init - Initialise the aurora buffer.
 */
void pic_aurora_init(pic_aurora_t *data)
{
    memset(data, 0, sizeof(*data));
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * parse_aurora_json - Parse the SWPC OVATION aurora JSON.
 *
 * The JSON contains a "coordinates" array of [lon, lat, prob] triples:
 *   {"coordinates": [[0,-90,3],[0,-89,0],[0,-88,4],...]}
 *
 * Longitude: 0 to 359, Latitude: -90 to 90, Probability: 0 to 100.
 * We scan for the "coordinates" key then parse triples.
 */
static int parse_aurora_json(const char *json, pic_aurora_t *data)
{
    const char *p;
    int count = 0;

    /* Find the coordinates array */
    p = strstr(json, "\"coordinates\"");
    if (!p) {
        printf("aurora: no coordinates key in JSON\n");
        return 0;
    }
    p = strchr(p, '[');
    if (!p) return 0;
    p++; /* Skip outer [ */

    pthread_mutex_lock(&data->mutex);
    memset(data->prob, 0, sizeof(data->prob));

    /* Parse [lon, lat, prob] triples */
    while (*p) {
        double lon_d, lat_d, prob_d;
        int lon, lat_idx;
        char *end;

        /* Find next inner [ */
        p = strchr(p, '[');
        if (!p) break;
        p++;

        /* Parse three numbers */
        lon_d = strtod(p, &end);
        if (end == p || *end != ',') break;
        p = end + 1;

        lat_d = strtod(p, &end);
        if (end == p || *end != ',') break;
        p = end + 1;

        prob_d = strtod(p, &end);
        if (end == p) break;
        p = end;

        /* Skip to closing ] */
        while (*p && *p != ']') p++;
        if (*p) p++;

        /* Validate and map to grid */
        if (lon_d < 0 || lon_d > 359 || lat_d < -90 || lat_d > 90)
            continue;
        if (prob_d < 0) prob_d = 0;
        if (prob_d > 100) prob_d = 100;

        lon = (int)lon_d;
        lat_idx = (int)(90.0 - lat_d);

        if (lon >= 0 && lon < PIC_AURORA_NX &&
            lat_idx >= 0 && lat_idx < PIC_AURORA_NY) {
            data->prob[lat_idx][lon] = (float)prob_d;
            count++;
        }
    }

    data->last_fetched = time(NULL);
    if (count > 0) data->valid = 1;

    pthread_mutex_unlock(&data->mutex);

    return count;
}

/*
 * aurora_thread_func - Background thread for fetching aurora data.
 */
static void *aurora_thread_func(void *arg)
{
    char *buf;

    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) {
        fprintf(stderr, "aurora: failed to allocate fetch buffer\n");
        return NULL;
    }

    if (!pic_wait_for_network("aurora", &aurora_running)) {
        free(buf);
        return NULL;
    }

    while (aurora_running) {
        int n;

        n = pic_fetch_to_buf(AURORA_URL, buf, MAX_RESPONSE);
        if (n > 0) {
            int count = parse_aurora_json(buf, cfg_data);
            printf("aurora: parsed %d grid points from SWPC\n", count);
        } else {
            printf("aurora: fetch failed, retrying in %ds\n",
                   AURORA_FETCH_INTERVAL);
        }

        /* Sleep in 1-second chunks */
        {
            int s;
            for (s = 0; s < AURORA_FETCH_INTERVAL && aurora_running; s++)
                sleep(1);
        }
    }

    free(buf);
    return NULL;
}

/*
 * pic_aurora_start - Start the SWPC fetch thread.
 */
int pic_aurora_start(pic_aurora_t *data)
{
    if (aurora_running) return 0;

    cfg_data = data;
    aurora_running = 1;

    if (pthread_create(&aurora_thread, NULL,
                       aurora_thread_func, NULL) != 0) {
        aurora_running = 0;
        fprintf(stderr, "aurora: failed to create thread\n");
        return -1;
    }

    printf("aurora: fetcher thread started (interval=%ds)\n",
           AURORA_FETCH_INTERVAL);
    return 0;
}

/*
 * pic_aurora_stop - Stop the background fetch thread.
 */
void pic_aurora_stop(void)
{
    if (!aurora_running) return;
    aurora_running = 0;
    pthread_join(aurora_thread, NULL);
    cfg_data = NULL;
    printf("aurora: fetcher thread stopped\n");
}
