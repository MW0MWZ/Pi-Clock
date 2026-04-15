/*
 * solarweather.c - NOAA SWPC solar weather data fetcher
 *
 * Runs a background thread that periodically fetches solar
 * indices from the NOAA Space Weather Prediction Center.
 * Uses wget (available on Alpine via busybox) to fetch JSON,
 * then does minimal parsing to extract numeric values.
 *
 * Endpoints used (all free, no auth):
 *   - SFI:        /products/summary/10cm-flux.json
 *   - K-index:    /json/planetary_k_index_1m.json (last entry)
 *   - Solar wind: /products/summary/solar-wind-speed.json
 *   - Bz/Bt:      /products/summary/solar-wind-mag-field.json
 *   - X-ray:      /json/goes/primary/xray-flares-latest.json
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "solarweather.h"
#include "config.h"
#include "fetch.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

/*
 * Tiered fetch intervals — balanced between data freshness and
 * being a good citizen to NOAA/NASA servers. TLS handshakes are
 * expensive on the single-core Pi Zero, so we keep things gentle.
 *
 * Tier 1 (30 min): Kp, solar wind, Bz/Bt, X-ray flares
 *                   These update every 1 minute at NOAA, but 30 min
 *                   is plenty to catch geomagnetic storm onsets for
 *                   a display device. Keeps TLS overhead minimal.
 *
 * Tier 2 (2 hr):   SDO solar disc images
 *                   NASA publishes new images every ~15 min, but the
 *                   visual difference is subtle. 2-hourly is fine.
 *
 * Tier 3 (6 hr):   SFI (10cm solar flux)
 *                   Measured once per day at ~20:00 UTC (DRAO Penticton).
 *                   Only changes once every 24 hours.
 */
#define FETCH_TICK        1800  /* Base tick: 30 minutes (seconds) */
#define FETCH_IMAGES_EVERY  4   /* 4 ticks = 2 hours */
#define FETCH_SFI_EVERY    12   /* 12 ticks = 6 hours */

static pthread_t solar_thread;
static volatile int solar_running = 0;
static pic_solar_data_t *solar_data;

/*
 * fetch_url_cached - Fetch a URL, caching the response to tmpfs.
 *
 * On success, writes the response to /tmp/pi-clock-cache/{cache_name}.
 * If the network fetch fails, reads from the cache file instead.
 * This means a service restart gets instant data without re-fetching.
 */
static int fetch_url_cached(const char *url, char *buf, int buf_size,
                            const char *cache_name)
{
    char path[128];
    int n;

    snprintf(path, sizeof(path), "/tmp/pi-clock-cache/%s", cache_name);

    /* Try network first */
    n = pic_fetch_to_buf(url, buf, buf_size);
    if (n > 0) {
        /* Cache the response */
        FILE *f = fopen(path, "w");
        if (f) {
            fwrite(buf, 1, n, f);
            fclose(f);
        }
        return n;
    }

    /* Network failed — try cache */
    {
        FILE *f = fopen(path, "r");
        if (!f) return -1;
        n = fread(buf, 1, buf_size - 1, f);
        fclose(f);
        if (n > 0) {
            buf[n] = '\0';
            printf("solar: using cached %s\n", cache_name);
            return n;
        }
    }

    return -1;
}

/*
 * json_find_value - Find a JSON key and extract its value.
 *
 * Very simple: finds "key": and returns the value after the colon.
 * Works for numbers and simple strings (no nested objects).
 * Returns pointer to value start, or NULL if not found.
 */
static const char *json_find_value(const char *json, const char *key)
{
    char pattern[64];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return NULL;

    p += strlen(pattern);

    /* Skip whitespace and colon */
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    return p;
}

/*
 * json_extract_int - Extract an integer value for a key.
 */
static int json_extract_int(const char *json, const char *key)
{
    const char *v = json_find_value(json, key);
    if (!v) return 0;
    return atoi(v);
}

/*
 * json_extract_double - Extract a double value for a key.
 */
static double json_extract_double(const char *json, const char *key)
{
    const char *v = json_find_value(json, key);
    if (!v) return 0.0;
    return atof(v);
}

/*
 * json_extract_string - Extract a string value for a key.
 */
static void json_extract_string(const char *json, const char *key,
                                char *out, int out_size)
{
    const char *v = json_find_value(json, key);
    int i = 0;

    out[0] = '\0';
    if (!v) return;

    /* Skip opening quote */
    if (*v == '"') v++;

    while (*v && *v != '"' && i < out_size - 1) {
        out[i++] = *v++;
    }
    out[i] = '\0';
}

/*
 * fetch_realtime - Tier 1: time-sensitive data (every 30 min).
 *
 * K-index, solar wind speed, Bz/Bt, and X-ray flares all update
 * every 1 minute at NOAA but 30-minute polling is sufficient to
 * catch geomagnetic storm onsets and flare events promptly.
 */
static void fetch_realtime(void)
{
    char buf[8192];
    double kp = 0;
    int wind_speed = 0;
    double bz = 0;
    int bt = 0;
    char xray[8] = "";

    printf("solar: fetching real-time data...\n");

    /* K-index — the endpoint returns an array of 1-minute readings.
     * We want only the most recent (last) entry. Strategy: scan
     * forward through all occurrences of "estimated_kp", keeping
     * a pointer to the last one found. Then extract the value.
     * The (last - 1) offset backs up one char so json_find_value()
     * can find the opening quote of the key. */
    if (fetch_url_cached("https://services.swpc.noaa.gov/json/planetary_k_index_1m.json",
                  buf, sizeof(buf), "noaa_kp.json") > 0) {
        const char *last = NULL;
        const char *p = buf;
        while ((p = strstr(p, "\"estimated_kp\"")) != NULL) {
            last = p;
            p++;
        }
        if (last && last > buf) {
            kp = json_extract_double(last - 1, "estimated_kp");
            printf("solar: Kp=%.2f\n", kp);
        }
    }

    /* Solar wind speed */
    if (fetch_url_cached("https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json",
                  buf, sizeof(buf), "noaa_wind.json") > 0) {
        wind_speed = json_extract_int(buf, "proton_speed");
        printf("solar: wind=%d km/s\n", wind_speed);
    }

    /* Bz / Bt — most important storm precursor. Sustained negative Bz
     * drives geomagnetic storms that degrade HF propagation. */
    if (fetch_url_cached("https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json",
                  buf, sizeof(buf), "noaa_mag.json") > 0) {
        bt = json_extract_int(buf, "bt");
        bz = json_extract_double(buf, "bz_gsm");
        printf("solar: Bt=%d Bz=%.1f\n", bt, bz);
    }

    /* X-ray flare class — effect is instantaneous (speed of light),
     * so we can't warn ahead, but 5-min polling shows ongoing events. */
    if (fetch_url_cached("https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json",
                  buf, sizeof(buf), "noaa_xray.json") > 0) {
        json_extract_string(buf, "max_class", xray, sizeof(xray));
        if (xray[0] == '\0') {
            json_extract_string(buf, "current_class", xray, sizeof(xray));
        }
        printf("solar: X-ray=%s\n", xray);
    }

    /* Update shared data */
    pthread_mutex_lock(&solar_data->mutex);
    if (kp > 0) solar_data->kp = kp;
    if (wind_speed > 0) solar_data->solar_wind_speed = wind_speed;
    solar_data->bt = bt;
    solar_data->bz = bz;
    if (xray[0]) strncpy(solar_data->xray_class, xray, sizeof(solar_data->xray_class) - 1);
    solar_data->last_updated = time(NULL);
    pthread_mutex_unlock(&solar_data->mutex);
}

/*
 * fetch_images - Tier 2: SDO solar disc images (every 30 min).
 *
 * NASA publishes new images every ~15 min, so 30-min polling
 * avoids redundant downloads while keeping the display current.
 */
static void fetch_images(void)
{
    static const struct {
        const char *code;   /* SDO image code */
        const char *label;  /* Display label */
    } sdo_images[] = {
        {"HMIIC",     "Visible"},       /* Colourised, sunspots */
        {"0171",      "171\xC3\x85"},   /* Corona, magnetic loops (Å) */
        {"0304",      "304\xC3\x85"},   /* Chromosphere, flares */
        {"0193",      "193\xC3\x85"},   /* Hot corona */
        {"211193171", "Composite"},      /* Multi-wavelength false colour */
    };
    int si;

    printf("solar: fetching SDO images...\n");

    for (si = 0; si < SDO_IMAGE_COUNT; si++) {
        char path[64], url[256];
        int ret;

        snprintf(path, sizeof(path), "/tmp/pi-clock-cache/sdo_%s.jpg",
                 sdo_images[si].code);
        snprintf(url, sizeof(url),
                 "https://sdo.gsfc.nasa.gov/assets/img/latest/"
                 "latest_256_%s.jpg",
                 sdo_images[si].code);

        ret = pic_fetch_to_file(url, path, 10);
        if (ret == 0) {
            cairo_surface_t *img = pic_image_load(path);
            if (img) {
                cairo_surface_t *old = NULL;
                pthread_mutex_lock(&solar_data->mutex);
                old = solar_data->sun_images[si];
                solar_data->sun_images[si] = img;
                solar_data->sun_labels[si] = sdo_images[si].label;
                solar_data->sun_image_count = SDO_IMAGE_COUNT;
                pthread_mutex_unlock(&solar_data->mutex);
                /* Destroy the old surface outside the lock so the
                 * render thread's solar applet isn't blocked. */
                if (old) cairo_surface_destroy(old);
            }
        }
    }
    printf("solar: loaded %d SDO images\n", SDO_IMAGE_COUNT);
}

/*
 * fetch_sfi - Tier 3: daily solar flux index (every 6 hours).
 *
 * DRAO Penticton measures once per day at ~20:00 UTC. The value
 * only changes once every 24 hours, so 6-hourly polling is plenty.
 */
static void fetch_sfi(void)
{
    char buf[8192];
    int sfi = 0;

    printf("solar: fetching SFI...\n");

    if (fetch_url_cached("https://services.swpc.noaa.gov/products/summary/10cm-flux.json",
                  buf, sizeof(buf), "noaa_sfi.json") > 0) {
        sfi = json_extract_int(buf, "flux");
        printf("solar: SFI=%d\n", sfi);
    }

    if (sfi > 0) {
        pthread_mutex_lock(&solar_data->mutex);
        solar_data->sfi = sfi;
        pthread_mutex_unlock(&solar_data->mutex);
    }
}

/*
 * solar_thread_func - Background fetch loop with tiered intervals.
 *
 * Uses a tick counter (1 tick = FETCH_TICK seconds = 30 min).
 * Every tick:       fetch real-time data (Kp, wind, Bz, X-ray)
 * Every 4 ticks:    fetch SDO images (2 hours)
 * Every 12 ticks:   fetch SFI (6 hours)
 *
 * On first run after network comes up, all tiers fire immediately.
 */
static void *solar_thread_func(void *arg)
{
    int tick = 0;

    (void)arg;

    /* Lower priority so TLS handshakes don't starve the renderer.
     * On Pi Zero, each HTTPS fetch takes seconds of CPU. */
    nice(15);

    /* Ensure tmpfs cache directory exists */
    mkdir("/tmp/pi-clock-cache", 0700);

    /* Wait for network before first fetch */
    if (!pic_wait_for_network("solar", &solar_running)) {
        printf("solar: will retry in %d seconds\n", FETCH_TICK);
    } else {
        /* First run — fetch everything */
        fetch_realtime();
        fetch_images();
        fetch_sfi();
        printf("solar: initial fetch complete\n");
    }

    while (solar_running) {
        int elapsed = 0;

        /* Sleep in 5-second chunks so we can stop promptly */
        while (solar_running && elapsed < FETCH_TICK) {
            sleep(5);
            elapsed += 5;
        }

        if (!solar_running) break;

        tick++;

        /* Tier 1: real-time data every tick (30 min) */
        fetch_realtime();

        /* Tier 2: SDO images every 4 ticks (2 hours) */
        if (tick % FETCH_IMAGES_EVERY == 0) {
            fetch_images();
        }

        /* Tier 3: SFI every 12 ticks (6 hours) */
        if (tick % FETCH_SFI_EVERY == 0) {
            fetch_sfi();
        }
    }

    return NULL;
}

/* Initialise solar data. All values are 0 until the first NOAA fetch. */
void pic_solar_init(pic_solar_data_t *data)
{
    memset(data, 0, sizeof(*data));
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * pic_solar_start - Launch the background NOAA fetch thread.
 *
 * Fetches all tiers immediately after network is available, then
 * repeats on a tiered schedule: real-time every 30 min, images
 * every 2 hours, SFI every 6 hours. Thread is detached.
 * Returns 0 on success, -1 if thread creation fails.
 */
int pic_solar_start(pic_solar_data_t *data)
{
    if (solar_running) return 0;

    solar_data = data;
    solar_running = 1;

    if (pthread_create(&solar_thread, NULL, solar_thread_func, NULL) != 0) {
        solar_running = 0;
        fprintf(stderr, "solar: failed to create thread\n");
        return -1;
    }

    /* Thread is joinable — pic_solar_stop() calls pthread_join */
    printf("solar: fetch thread started (realtime=%ds, images=%ds, sfi=%ds)\n",
           FETCH_TICK, FETCH_TICK * FETCH_IMAGES_EVERY,
           FETCH_TICK * FETCH_SFI_EVERY);
    return 0;
}

/* Signal the fetch thread to stop. Exits after current sleep cycle. */
void pic_solar_stop(void)
{
    if (!solar_running) return;
    solar_running = 0;
    pthread_join(solar_thread, NULL);
}
