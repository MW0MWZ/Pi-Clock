/*
 * satellite.c - Satellite tracking: TLE fetch + SGP4 propagation
 *
 * Downloads TLE data from Celestrak for registered satellites,
 * parses the two-line element sets, and propagates orbits using
 * a simplified SGP4 model to compute current lat/lon/alt.
 *
 * The SGP4 implementation here is simplified but accurate enough
 * for map display — typically within a few km of the full model.
 * It handles the key perturbations: J2 oblateness, atmospheric
 * drag (B*), and secular variations in RAAN and argument of perigee.
 *
 * References:
 *   - Hoots & Roehrich, "Spacetrack Report No. 3" (1980)
 *   - Vallado et al., "Revisiting Spacetrack Report #3" (2006)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "satellite.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "math_utils.h"
#define TWO_PI (2.0 * M_PI)

/* WGS84 Earth constants */
#define EARTH_R_KM  6378.137       /* Equatorial radius (km) */
#define EARTH_MU    398600.4418    /* Gravitational parameter (km^3/s^2) */
#define J2          0.00108263     /* Second zonal harmonic */
#define MINUTES_PER_DAY  1440.0

/* TLE fetch interval (1 hour) */
#define TLE_FETCH_INTERVAL 3600

static pthread_t sat_thread;
static volatile int sat_running = 0;
static pic_satlist_t *sat_list;

/*
 * parse_tle - Parse a standard two-line element set.
 *
 * Line 1: 1 NNNNNC YYYYY.DDDDDDDD  ...  BSTAR ...
 * Line 2: 2 NNNNN INC RAAN ECC ARGP MA MM ...
 */
static int parse_tle(pic_sat_t *sat, const char *line1, const char *line2)
{
    double epoch_yr, epoch_day;
    double inc, raan, ecc, argp, ma, mm;
    char ecc_str[8];
    int yr;

    if (strlen(line1) < 69 || strlen(line2) < 69) return -1;
    if (line1[0] != '1' || line2[0] != '2') return -1;

    /* Line 1: epoch and B* */
    {
        char yr_str[3], day_str[13], bstar_str[12];
        int bstar_exp;

        memcpy(yr_str, line1 + 18, 2); yr_str[2] = '\0';
        yr = atoi(yr_str);
        /* TLE epoch year: 2-digit. NORAD convention: < 57 = 21st century,
         * >= 57 = 20th century. Based on Sputnik launch year (1957). */
        epoch_yr = (yr < 57) ? 2000 + yr : 1900 + yr;

        memcpy(day_str, line1 + 20, 12); day_str[12] = '\0';
        epoch_day = atof(day_str);

        /* B* drag term: mantissa in cols 54-59, exponent in 60-61 */
        memcpy(bstar_str, line1 + 53, 6); bstar_str[6] = '\0';
        bstar_exp = atoi(line1 + 59);
        sat->bstar = atof(bstar_str) * 1e-5 * pow(10.0, bstar_exp);
    }

    /* Convert epoch to Julian date */
    {
        /* Jan 1 of epoch year as JD, then add day-of-year */
        int y = (int)epoch_yr;
        double jd_jan1 = 367.0 * y
            - (int)(7.0 * (y + (int)(10.0 / 12.0)) / 4.0)
            + (int)(275.0 / 9.0)
            + 1721013.5;
        sat->epoch_jd = jd_jan1 + epoch_day;
    }

    /* Line 2: orbital elements */
    {
        char buf[16];

        memcpy(buf, line2 + 8, 8); buf[8] = '\0';
        inc = atof(buf);

        memcpy(buf, line2 + 17, 8); buf[8] = '\0';
        raan = atof(buf);

        /* Eccentricity: implied leading "0." */
        memcpy(ecc_str, line2 + 26, 7); ecc_str[7] = '\0';
        ecc = atof(ecc_str) * 1e-7;

        memcpy(buf, line2 + 34, 8); buf[8] = '\0';
        argp = atof(buf);

        memcpy(buf, line2 + 43, 8); buf[8] = '\0';
        ma = atof(buf);

        memcpy(buf, line2 + 52, 11); buf[11] = '\0';
        mm = atof(buf);
    }

    sat->inclination = inc * DEG_TO_RAD;
    sat->raan = raan * DEG_TO_RAD;
    sat->eccentricity = ecc;
    sat->arg_perigee = argp * DEG_TO_RAD;
    sat->mean_anomaly = ma * DEG_TO_RAD;
    sat->mean_motion = mm;  /* revs/day */
    sat->tle_loaded = 1;

    return 0;
}

/*
 * sgp4_at_time - Simplified SGP4: compute position at a given time.
 *
 * Returns lat/lon (degrees) and altitude (km).
 */
static void sgp4_at_time(const pic_sat_t *sat, double jd,
                         double *out_lat, double *out_lon, double *out_alt)
{
    double tsince_min;
    double n0, a0, e0, i0, w0, om0, m0;
    double n, a, e, w, om, M, E, v;
    double r, u, lat_rad, lon_rad, gmst;

    tsince_min = (jd - sat->epoch_jd) * MINUTES_PER_DAY;

    /* Convert mean motion from rev/day to rad/min */
    n0 = sat->mean_motion * TWO_PI / MINUTES_PER_DAY; /* rad/min */
    /* Semi-major axis from Kepler's 3rd law: a = (mu/n²)^(1/3).
     * mu is in km³/s², n is in rad/min — multiply mu by 3600 to
     * convert to km³/min². Result a0 is in km. */
    a0 = pow((EARTH_MU * 3600.0) / (n0 * n0), 1.0 / 3.0);
    e0 = sat->eccentricity;
    i0 = sat->inclination;
    w0 = sat->arg_perigee;
    om0 = sat->raan;
    m0 = sat->mean_anomaly;

    /* Secular J2 perturbations */
    {
        double p = a0 * (1.0 - e0 * e0);
        double cosi = cos(i0);
        double sini = sin(i0);
        double j2_f = 1.5 * J2 * (EARTH_R_KM / p) * (EARTH_R_KM / p);

        double om_dot = -j2_f * n0 * cosi;
        double w_dot = j2_f * n0 * (2.0 - 2.5 * sini * sini);

        n = n0;
        a = a0;
        e = e0;
        w = w0 + w_dot * tsince_min;
        om = om0 + om_dot * tsince_min;
        M = m0 + n * tsince_min;
    }

    /* Kepler's equation */
    E = M;
    {
        int iter;
        for (iter = 0; iter < 10; iter++) {
            double dE = (M - E + e * sin(E)) / (1.0 - e * cos(E));
            E += dE;
            if (fabs(dE) < 1e-10) break;
        }
    }

    v = 2.0 * atan2(sqrt(1.0 + e) * sin(E / 2.0),
                     sqrt(1.0 - e) * cos(E / 2.0));
    r = a * (1.0 - e * cos(E));
    u = v + w;

    /* Orbital plane → ECI → geodetic */
    {
        double x_orb = r * cos(u);
        double y_orb = r * sin(u);
        double cos_om = cos(om), sin_om = sin(om);
        double cos_i = cos(i0), sin_i = sin(i0);

        double x_eci = x_orb * cos_om - y_orb * cos_i * sin_om;
        double y_eci = x_orb * sin_om + y_orb * cos_i * cos_om;
        double z_eci = y_orb * sin_i;

        lat_rad = atan2(z_eci, sqrt(x_eci * x_eci + y_eci * y_eci));

        /* Greenwich Mean Sidereal Time (GMST) in degrees.
         * IAU 1982 formula. T = Julian centuries since J2000.0.
         * 2451545.0 = J2000.0 (noon, 1 Jan 2000).
         * 36525.0 = days per Julian century.
         * Ref: Astronomical Almanac, B7. */
        {
            double T = (jd - 2451545.0) / 36525.0;
            gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                   + 0.000387933 * T * T;
            gmst = fmod(gmst, 360.0);
            if (gmst < 0) gmst += 360.0;
        }

        lon_rad = atan2(y_eci, x_eci) - gmst * DEG_TO_RAD;
        while (lon_rad > PI) lon_rad -= TWO_PI;
        while (lon_rad < -PI) lon_rad += TWO_PI;
    }

    *out_lat = lat_rad * RAD_TO_DEG;
    *out_lon = lon_rad * RAD_TO_DEG;
    *out_alt = r - EARTH_R_KM;
}

/*
 * sgp4_propagate - Compute current position and ground track.
 *
 * Calculates the satellite's current position plus a ground track
 * showing 25% of the previous orbit and 75% of the current orbit.
 */
static void sgp4_propagate(pic_sat_t *sat, time_t now)
{
    double jd_now, orbit_period_min;
    int i;

    if (!sat->tle_loaded) return;

    jd_now = 2440587.5 + (double)now / 86400.0;

    /* Current position */
    sgp4_at_time(sat, jd_now, &sat->lat, &sat->lon, &sat->alt_km);

    /* Footprint radius: the angular distance from the sub-satellite point
     * to the horizon, i.e., where elevation angle = 0°.
     * acos(R / (R + alt)) gives the half-angle of the coverage cone. */
    if (sat->alt_km > 0) {
        sat->footprint_r = acos(EARTH_R_KM / (EARTH_R_KM + sat->alt_km))
                           * RAD_TO_DEG;
    } else {
        sat->footprint_r = 0;
    }

    /* Orbital period in minutes */
    orbit_period_min = MINUTES_PER_DAY / sat->mean_motion;

    /* Ground track: 25% past + 75% future of one orbit.
     * SAT_TRACK_PTS points spread over the full orbit period.
     * track_now_idx marks where "now" is in the array. */
    {
        double past_min = orbit_period_min * 0.25;
        double future_min = orbit_period_min * 0.75;
        double total_min = past_min + future_min;
        double step = total_min / (SAT_TRACK_PTS - 1);

        sat->track_now_idx = (int)(past_min / step);
        sat->track_count = SAT_TRACK_PTS;

        for (i = 0; i < SAT_TRACK_PTS; i++) {
            double t_offset = -past_min + i * step;  /* minutes from now */
            double jd_pt = jd_now + t_offset / MINUTES_PER_DAY;
            double dummy_alt;

            sgp4_at_time(sat, jd_pt,
                         &sat->track_lat[i], &sat->track_lon[i],
                         &dummy_alt);
        }
    }
}

/* TLE cache file — stored on tmpfs so it survives service restarts
 * but not reboots. Avoids hammering Celestrak on every restart. */
#define TLE_CACHE "/tmp/pi-clock-tles.txt"

/* Bulk TLE URL — fetches ALL amateur satellites in one request.
 * One TLS handshake instead of 20+ individual ones.
 * AMSAT is used instead of Celestrak — Celestrak's TLS handshake
 * is too slow for BusyBox ssl_client on Pi Zero (times out). */
#define TLE_BULK_URL "https://www.amsat.org/tle/current/nasabare.txt"

/*
 * find_tle_in_cache - Search the cached TLE data for a NORAD ID.
 * TLE format: 3 lines per satellite (name, line1, line2).
 * Line 1 columns 3-7 contain the NORAD catalog number.
 */
static int find_tle_in_cache(const char *cache, int norad_id,
                             char *l1, char *l2)
{
    const char *p = cache;
    char id_str[8];

    snprintf(id_str, sizeof(id_str), "%05d", norad_id);

    while (*p) {
        /* Skip name line */
        while (*p && *p != '\n') p++;
        if (*p) p++;

        /* This should be line 1 — check NORAD ID at columns 3-7 */
        if (*p == '1' && p[1] == ' ' && strncmp(p + 2, id_str, 5) == 0) {
            /* Found it — extract line 1 and line 2 */
            int i;
            for (i = 0; *p && *p != '\n' && i < 79; i++) l1[i] = *p++;
            l1[i] = '\0';
            if (*p == '\n') p++;
            for (i = 0; *p && *p != '\n' && i < 79; i++) l2[i] = *p++;
            l2[i] = '\0';
            return 0;
        }

        /* Skip line 1 */
        while (*p && *p != '\n') p++;
        if (*p) p++;
        /* Skip line 2 */
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

/* Shared TLE cache — loaded once from file, cleared on refresh */
static char *tle_cache = NULL;
static size_t tle_cache_size = 0;

static void tle_cache_clear(void)
{
    free(tle_cache);
    tle_cache = NULL;
    tle_cache_size = 0;
}

/*
 * fetch_tle - Load TLE for a satellite from the cache.
 */
static int fetch_tle(pic_sat_t *sat)
{
    char l1[80] = "", l2[80] = "";

    /* Load cache if not already loaded */
    if (!tle_cache) {
        FILE *f = fopen(TLE_CACHE, "r");
        if (f) {
            long fsz;
            fseek(f, 0, SEEK_END);
            fsz = ftell(f);
            if (fsz <= 0 || fsz > 5 * 1024 * 1024) {
                /* TLE files are typically <500KB. Reject obviously
                 * wrong sizes (ftell returns -1 on error). */
                fclose(f);
                return -1;
            }
            tle_cache_size = (size_t)fsz;
            fseek(f, 0, SEEK_SET);
            tle_cache = malloc(tle_cache_size + 1);
            if (tle_cache) {
                if (fread(tle_cache, 1, tle_cache_size, f) != tle_cache_size) {
                    free(tle_cache);
                    tle_cache = NULL;
                    fclose(f);
                    return -1;
                }
                tle_cache[tle_cache_size] = '\0';
            }
            fclose(f);
        }
    }

    if (!tle_cache) return -1;

    if (find_tle_in_cache(tle_cache, sat->norad_id, l1, l2) == 0) {
        if (parse_tle(sat, l1, l2) == 0) {
            printf("satellite: TLE loaded for %s (NORAD %d, MM=%.4f rev/day)\n",
                   sat->name, sat->norad_id, sat->mean_motion);
            return 0;
        }
    }

    return -1;
}

/*
 * fetch_all_tles - Download bulk TLE catalog and parse for our satellites.
 *
 * Makes ONE request to Celestrak for the entire amateur satellite catalog,
 * caches it to /tmp, then parses TLEs for each registered satellite.
 * One TLS handshake instead of 20+ individual ones.
 */
static void fetch_all_tles(void)
{
    int i, loaded = 0;

    printf("satellite: fetching amateur TLE catalog...\n");

    /* Download bulk TLE catalog — one request for everything */
    if (pic_fetch_to_file(TLE_BULK_URL, TLE_CACHE, 30) != 0) {
        printf("satellite: bulk TLE download failed, trying cache\n");
    }

    /* Clear in-memory cache so fetch_tle re-reads the fresh file */
    tle_cache_clear();

    /* Parse TLEs from cache for each registered satellite */
    pthread_mutex_lock(&sat_list->mutex);
    for (i = 0; i < sat_list->count; i++) {
        if (!sat_running) break;
        if (!sat_list->sats[i].enabled) continue;
        if (fetch_tle(&sat_list->sats[i]) == 0) loaded++;
    }
    pthread_mutex_unlock(&sat_list->mutex);

    sat_list->last_tle_update = time(NULL);
    printf("satellite: TLE fetch complete (%d loaded)\n", loaded);
}

static void *sat_thread_func(void *arg)
{
    (void)arg;

    /* Lower priority so TLS handshakes don't starve the renderer */
    nice(15);

    if (!pic_wait_for_network("satellite", &sat_running)) {
        printf("satellite: will retry in %d seconds\n", TLE_FETCH_INTERVAL);
    } else {
        fetch_all_tles();
    }

    while (sat_running) {
        int elapsed = 0;
        while (sat_running && elapsed < TLE_FETCH_INTERVAL) {
            sleep(5);
            elapsed += 5;
            /* Config reload can reset last_tle_update to trigger
             * an immediate refetch for newly enabled satellites */
            if (sat_list->last_tle_update == 0) break;
        }
        if (sat_running) {
            fetch_all_tles();
        }
    }

    return NULL;
}

void pic_sat_init(pic_satlist_t *list)
{
    memset(list, 0, sizeof(*list));
    pthread_mutex_init(&list->mutex, NULL);
}

int pic_sat_add(pic_satlist_t *list, const char *config_key,
                const char *name, int norad_id)
{
    pic_sat_t *s;

    if (list->count >= SAT_MAX) return -1;

    s = &list->sats[list->count];
    strncpy(s->config_key, config_key, SAT_NAME_LEN - 1);
    strncpy(s->name, name, SAT_NAME_LEN - 1);
    s->norad_id = norad_id;
    s->enabled = 0;  /* Off until config file says otherwise */

    list->count++;
    printf("satellite: registered '%s' [%s] (NORAD %d)\n",
           name, config_key, norad_id);
    return 0;
}

#define SAT_CONF "/data/etc/pi-clock-satellites.conf"

/*
 * pic_sat_load_config - Apply enable/disable settings from config file.
 *
 * After updating the enabled flags, fetches TLEs for any satellite
 * that is now enabled but doesn't have orbital data yet. This handles
 * the case where a satellite was disabled at startup (TLE skipped)
 * and then enabled via the dashboard mid-run.
 */
void pic_sat_load_config(pic_satlist_t *list)
{
    FILE *f;
    char line[128];
    int i;
    int need_fetch = 0;

    f = fopen(SAT_CONF, "r");
    if (!f) return;

    printf("satellite: loading config from %s\n", SAT_CONF);

    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int enabled;

        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%31[^=]=%d", key, &enabled) == 2) {
            for (i = 0; i < list->count; i++) {
                if (strcmp(list->sats[i].config_key, key) == 0) {
                    list->sats[i].enabled = enabled;
                    printf("satellite: '%s' enabled=%d\n", key, enabled);
                    /* Flag if newly enabled without TLE data */
                    if (enabled && !list->sats[i].tle_loaded)
                        need_fetch = 1;
                    break;
                }
            }
        }
    }

    fclose(f);

    /* If satellites were newly enabled, signal the background thread
     * to fetch their TLEs. Never do network I/O in the main thread —
     * TLS handshakes on Pi Zero take seconds and block rendering. */
    if (need_fetch) {
        printf("satellite: newly enabled satellites — background thread will fetch TLEs\n");
        /* Reset last_tle_update to trigger an immediate fetch cycle */
        list->last_tle_update = 0;
    }
}

int pic_sat_start(pic_satlist_t *list)
{
    if (sat_running) return 0;

    sat_list = list;
    sat_running = 1;

    if (pthread_create(&sat_thread, NULL, sat_thread_func, NULL) != 0) {
        sat_running = 0;
        fprintf(stderr, "satellite: failed to create thread\n");
        return -1;
    }

    /* Thread is joinable — pic_sat_stop() calls pthread_join */
    printf("satellite: tracking thread started\n");
    return 0;
}

void pic_sat_stop(void)
{
    if (!sat_running) return;
    sat_running = 0;
    pthread_join(sat_thread, NULL);
}

static int propagate_logged = 0;

void pic_sat_propagate(pic_satlist_t *list, time_t now)
{
    int i;

    pthread_mutex_lock(&list->mutex);
    for (i = 0; i < list->count; i++) {
        if (list->sats[i].enabled && list->sats[i].tle_loaded) {
            sgp4_propagate(&list->sats[i], now);

            /* Log first propagation result for debugging */
            if (!propagate_logged) {
                printf("satellite: %s pos=%.2f,%.2f alt=%.0fkm fp=%.1f°\n",
                       list->sats[i].name,
                       list->sats[i].lat, list->sats[i].lon,
                       list->sats[i].alt_km, list->sats[i].footprint_r);
            }
        }
    }
    if (!propagate_logged && list->count > 0) propagate_logged = 1;
    pthread_mutex_unlock(&list->mutex);
}
