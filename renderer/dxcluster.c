/*
 * dxcluster.c - DX cluster telnet client
 *
 * Runs a background thread that:
 *   1. Connects to a DX cluster via TCP
 *   2. Sends the user's callsign for identification
 *   3. Reads spot lines and parses them
 *   4. Resolves spotter and DX callsign locations via cty.dat
 *   5. Adds spots to the shared spot list for rendering
 *
 * Reconnects automatically on disconnect with exponential backoff.
 * Sends a keepalive every 3 minutes to prevent timeout.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "dxcluster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

/* Keepalive interval in seconds */
#define KEEPALIVE_INTERVAL 180

/* Maximum reconnect backoff in seconds */
#define MAX_BACKOFF 300

/* Thread state */
static pthread_t cluster_thread;
static volatile int cluster_running = 0;
static int cluster_fd = -1;
static pthread_mutex_t fd_mu = PTHREAD_MUTEX_INITIALIZER;

/* Connection parameters (set before thread starts) */
static char cfg_host[128];
static int cfg_port;
static char cfg_callsign[16];
static char cfg_home_prefix[16];
static pic_spotlist_t *cfg_spots;
static pic_ctydat_t *cfg_db;

/*
 * parse_spot_line - Parse a standard DX cluster spot line.
 *
 * Format: "DX de SPOTTER:  FREQ.F DXCALL  COMMENT  HHMMZ"
 *
 * Returns 1 if the line was a valid spot, 0 otherwise.
 */
static int parse_spot_line(const char *line, pic_dxspot_t *spot)
{
    char spotter[32], dx_call[32], comment[64];
    double freq;

    if (strncasecmp(line, "DX de ", 6) != 0) {
        return 0;
    }

    if (sscanf(line + 6, "%31[^:]:%lf %31s %63[^\n]",
               spotter, &freq, dx_call, comment) < 3) {
        return 0;
    }

    /* Skip /MM and /AM callsigns (no fixed location) */
    if (strstr(dx_call, "/MM") || strstr(dx_call, "/AM") ||
        strstr(dx_call, "/mm") || strstr(dx_call, "/am")) {
        return 0;
    }

    /* Fill in the spot structure */
    memset(spot, 0, sizeof(*spot));
    strncpy(spot->spotter, spotter, sizeof(spot->spotter) - 1);
    strncpy(spot->dx_call, dx_call, sizeof(spot->dx_call) - 1);
    spot->freq_khz = freq;
    strncpy(spot->comment, comment, sizeof(spot->comment) - 1);
    spot->timestamp = time(NULL);
    spot->band_index = pic_freq_to_band(freq);
    spot->active = 1;

    return 1;
}

/* Home QTH and filtering config — set during startup */
static double home_lat = 0, home_lon = 0;
static double max_spot_distance = 1000.0;        /* Miles, 0 = show all */
static unsigned int band_mask = 0xFFFF;           /* Bitmask: bit N = band N enabled */
static pthread_mutex_t filter_mu = PTHREAD_MUTEX_INITIALIZER;

#include "math_utils.h"
#define EARTH_RADIUS_MI 3959.0

/*
 * great_circle_distance_mi - Distance between two points in miles.
 */
static double great_circle_distance_mi(double lat1, double lon1,
                                       double lat2, double lon2)
{
    double phi1 = lat1 * DEG_TO_RAD;
    double phi2 = lat2 * DEG_TO_RAD;
    double dlat = (lat2 - lat1) * DEG_TO_RAD;
    double dlon = (lon2 - lon1) * DEG_TO_RAD;
    double a, c;

    a = sin(dlat/2) * sin(dlat/2) +
        cos(phi1) * cos(phi2) * sin(dlon/2) * sin(dlon/2);
    c = 2 * atan2(sqrt(a), sqrt(1 - a));

    return EARTH_RADIUS_MI * c;
}

/*
 * resolve_spot_locations - Look up coordinates and filter by distance.
 *
 * Returns 1 if the spot should be shown (spotter or DX is within
 * MAX_SPOT_DISTANCE of the home QTH), 0 if it should be discarded.
 */
static int resolve_spot_locations(pic_dxspot_t *spot)
{
    const pic_country_t *spotter_country;
    double spotter_dist, dx_dist;
    unsigned int cur_band_mask;
    double cur_max_distance;

    if (!cfg_db) return 0;

    /* Snapshot filter state under the mutex so we see consistent
     * values. max_spot_distance is a 64-bit double — not atomic
     * on 32-bit ARM without the lock. */
    pthread_mutex_lock(&filter_mu);
    cur_band_mask = band_mask;
    cur_max_distance = max_spot_distance;
    pthread_mutex_unlock(&filter_mu);

    /* Use lookup_location to get per-district coordinates from
     * BigCTY rather than just the country centre. This places
     * W1xxx in New England and W6xxx in California, rather than
     * both pinned to the centre of the US. */
    spotter_country = pic_ctydat_lookup_location(cfg_db, spot->spotter,
                                                  &spot->spotter_lat,
                                                  &spot->spotter_lon);
    /* Return value unused — we only need the lat/lon side effect */
    (void)pic_ctydat_lookup_location(cfg_db, spot->dx_call,
                                     &spot->dx_lat,
                                     &spot->dx_lon);

    if (spotter_country) {
        if (cfg_home_prefix[0] &&
            strcmp(spotter_country->prefix, cfg_home_prefix) == 0) {
            spot->is_home_country = 1;
        }
    }

    /* Filter by band — skip if this band is disabled */
    if (spot->band_index >= 0 && spot->band_index < 32 &&
        !(cur_band_mask & (1u << spot->band_index))) {
        return 0;
    }

    /* Filter by distance — 0 means show all */
    if (cur_max_distance <= 0) return 1;
    if (home_lat == 0 && home_lon == 0) return 1;

    spotter_dist = great_circle_distance_mi(home_lat, home_lon,
                                             spot->spotter_lat, spot->spotter_lon);
    dx_dist = great_circle_distance_mi(home_lat, home_lon,
                                        spot->dx_lat, spot->dx_lon);

    return (spotter_dist <= cur_max_distance ||
            dx_dist <= cur_max_distance) ? 1 : 0;
}

/*
 * tcp_connect - Connect to the cluster server.
 * Returns socket fd on success, -1 on failure.
 */
static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);

    printf("dxcluster: resolving %s...\n", host);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        printf("dxcluster: DNS lookup failed for %s\n", host);
        return -1;
    }
    printf("dxcluster: DNS resolved, attempting connect...\n");

    fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int flags, ret;
        fd_set wfds;
        struct timeval tv;

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* Set non-blocking for connect with timeout */
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            /* Connected immediately */
            fcntl(fd, F_SETFL, flags); /* Back to blocking */
            break;
        }

        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        /* Wait for connect with 10-second timeout */
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                /* Connected */
                fcntl(fd, F_SETFL, flags); /* Back to blocking */
                break;
            }
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/*
 * cluster_thread_func - Background thread that maintains the
 * DX cluster connection and processes incoming spots.
 */
static void *cluster_thread_func(void *arg)
{
    int backoff = 5;

    (void)arg;

    while (cluster_running) {
        int fd;
        char line[512];
        time_t last_keepalive;
        printf("dxcluster: connecting to %s:%d\n", cfg_host, cfg_port);

        fd = tcp_connect(cfg_host, cfg_port);
        if (fd < 0) {
            printf("dxcluster: connection failed, retry in %ds\n", backoff);
            sleep(backoff);
            if (backoff < MAX_BACKOFF) backoff *= 2;
            continue;
        }

        /* Clear the connect timeout — recv should block indefinitely
         * waiting for spots (they arrive at irregular intervals).
         * We'll use KEEPALIVE_INTERVAL to detect dead connections. */
        {
            struct timeval tv;
            tv.tv_sec = KEEPALIVE_INTERVAL + 60; /* generous read timeout */
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        pthread_mutex_lock(&fd_mu);
        cluster_fd = fd;
        pthread_mutex_unlock(&fd_mu);
        printf("dxcluster: connected to %s:%d\n", cfg_host, cfg_port);
        backoff = 5;
        last_keepalive = time(NULL);

        /* Send callsign immediately */
        {
            char login[32];
            snprintf(login, sizeof(login), "%s\r\n", cfg_callsign);
            send(fd, login, strlen(login), 0);
            printf("dxcluster: sent callsign %s\n", cfg_callsign);
        }

        /*
         * Manual line assembler: recv() may return partial lines or
         * multiple lines in one call. We accumulate characters into
         * line[] until we see '\n', then process the complete line.
         * line_pos tracks bytes accumulated so far.
         *
         * We avoid fdopen/fgets because musl's stdio doesn't handle
         * sockets reliably after send() (drops the connection).
         */
        {
            char buf[4096];
            int line_pos = 0;
            int connected = 1;

            while (cluster_running && connected) {
                ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

                if (n <= 0) {
                    printf("dxcluster: recv returned %zd (errno=%d)\n",
                           n, errno);
                    connected = 0;
                    break;
                }

                /* Process received bytes, extracting complete lines */
                {
                    int bi;
                    for (bi = 0; bi < n; bi++) {
                        char c = buf[bi];

                        if (c == '\n') {
                            /* End of line — process it */
                            line[line_pos] = '\0';

                            /* Strip trailing \r */
                            if (line_pos > 0 && line[line_pos-1] == '\r') {
                                line[--line_pos] = '\0';
                            }

                            /* Parse DX spots */
                            if (line_pos > 0) {
                                pic_dxspot_t spot;
                                if (parse_spot_line(line, &spot)) {
                                    /* Always push to the live feed */
                                    {
                                        char feed_line[80];
                                        snprintf(feed_line, sizeof(feed_line),
                                                 "%-9s %8.1f  %-10s",
                                                 spot.spotter, spot.freq_khz,
                                                 spot.dx_call);
                                        pic_spotlist_feed_push(cfg_spots, feed_line);
                                    }

                                    /* Add to map if within distance/band filter */
                                    if (resolve_spot_locations(&spot)) {
                                        pic_spotlist_add(cfg_spots, &spot);
                                        printf("dxcluster: spot %s -> %s on %.1f kHz\n",
                                               spot.spotter, spot.dx_call,
                                               spot.freq_khz);
                                    }
                                }
                            }

                            line_pos = 0;
                        } else if (line_pos < (int)sizeof(line) - 1) {
                            line[line_pos++] = c;
                        }
                    }
                }

                /* Keepalive */
                if (time(NULL) - last_keepalive > KEEPALIVE_INTERVAL) {
                    send(fd, "\r\n", 2, 0);
                    last_keepalive = time(NULL);
                }
            }
        }

        printf("dxcluster: disconnected\n");
        pthread_mutex_lock(&fd_mu);
        cluster_fd = -1;
        pthread_mutex_unlock(&fd_mu);
        close(fd);

        if (cluster_running) {
            int s;
            printf("dxcluster: reconnecting in %ds\n", backoff);
            /* Sleep in 1s chunks so stop flag is checked promptly */
            for (s = 0; s < backoff && cluster_running; s++)
                sleep(1);
            if (backoff < MAX_BACKOFF) backoff *= 2;
        }
    }

    return NULL;
}

/*
 * pic_dxcluster_start - Start the DX cluster background thread.
 *
 * Reads filter settings from config, then starts a persistent telnet
 * connection. The thread reconnects on disconnect with exponential backoff.
 * Returns 0 on success, -1 if thread creation fails.
 */
int pic_dxcluster_start(const char *host, int port,
                        const char *callsign,
                        pic_spotlist_t *spots,
                        pic_ctydat_t *db,
                        const char *home_prefix,
                        double qth_lat, double qth_lon)
{
    if (cluster_running) return 0;

    strncpy(cfg_host, host, sizeof(cfg_host) - 1);
    cfg_port = port;
    strncpy(cfg_callsign, callsign, sizeof(cfg_callsign) - 1);
    if (home_prefix) {
        strncpy(cfg_home_prefix, home_prefix, sizeof(cfg_home_prefix) - 1);
    }
    home_lat = qth_lat;
    home_lon = qth_lon;
    cfg_spots = spots;

    /* Read DX filter settings from config */
    {
        FILE *cf = fopen("/data/etc/pi-clock-renderer.conf", "r");
        if (cf) {
            char cl[256];
            while (fgets(cl, sizeof(cl), cf)) {
                if (strncmp(cl, "DX_DISTANCE=", 12) == 0) {
                    max_spot_distance = atof(cl + 12);
                }
                if (strncmp(cl, "DX_BANDS=", 9) == 0) {
                    sscanf(cl + 9, "%x", &band_mask);
                }
                if (strncmp(cl, "DX_SPOT_AGE=", 12) == 0) {
                    pic_spot_max_age = atoi(cl + 12);
                    if (pic_spot_max_age < 60) pic_spot_max_age = 60;
                }
            }
            fclose(cf);
        }
        printf("dxcluster: distance=%.0f mi, band_mask=0x%04X\n",
               max_spot_distance, band_mask);
    }
    /* Store mask in spotlist so the DX layer legend can read it */
    if (cfg_spots) cfg_spots->band_mask = band_mask;
    cfg_db = db;

    cluster_running = 1;

    if (pthread_create(&cluster_thread, NULL, cluster_thread_func, NULL) != 0) {
        cluster_running = 0;
        fprintf(stderr, "dxcluster: failed to create thread\n");
        return -1;
    }

    /* Thread is joinable — pic_dxcluster_stop() calls pthread_join */
    printf("dxcluster: client thread started\n");

    return 0;
}

/* Signal the cluster thread to stop. Thread exits after current recv. */
void pic_dxcluster_stop(void)
{
    int fd;
    if (!cluster_running) return;
    cluster_running = 0;
    /* Unblock recv() immediately — without this, pthread_join
     * would stall for up to the SO_RCVTIMEO (240s). Mutex
     * prevents fd confusion if the thread closes the fd
     * between our read and the shutdown() call. */
    pthread_mutex_lock(&fd_mu);
    fd = cluster_fd;
    if (fd >= 0) {
        cluster_fd = -1;
        shutdown(fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&fd_mu);
    pthread_join(cluster_thread, NULL);
}

/*
 * pic_dxcluster_reload - Re-read filter settings from config.
 *
 * Called on SIGHUP/dashboard reload. Re-reads DX_DISTANCE, DX_BANDS,
 * DX_SPOT_AGE. If band mask changes, spots on disabled bands are
 * immediately expired. Connection is NOT restarted.
 */
void pic_dxcluster_reload(pic_spotlist_t *spots)
{
    unsigned int old_mask = band_mask;
    unsigned int new_mask = band_mask;
    double new_distance = max_spot_distance;
    int new_age = pic_spot_max_age;

    FILE *cf = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (cf) {
        char cl[256];
        while (fgets(cl, sizeof(cl), cf)) {
            if (strncmp(cl, "DX_DISTANCE=", 12) == 0)
                new_distance = atof(cl + 12);
            if (strncmp(cl, "DX_BANDS=", 9) == 0)
                sscanf(cl + 9, "%x", &new_mask);
            if (strncmp(cl, "DX_SPOT_AGE=", 12) == 0) {
                new_age = atoi(cl + 12);
                if (new_age < 60) new_age = 60;
            }
        }
        fclose(cf);
    }

    /* Write filter state under the mutex so the cluster thread
     * sees consistent values (double is not atomic on 32-bit ARM). */
    pthread_mutex_lock(&filter_mu);
    max_spot_distance = new_distance;
    band_mask = new_mask;
    pic_spot_max_age = new_age;
    pthread_mutex_unlock(&filter_mu);

    printf("dxcluster: reload distance=%.0f mi, bands=0x%04X, age=%ds\n",
           max_spot_distance, band_mask, pic_spot_max_age);

    /* Update mask in spotlist for the legend display */
    if (spots) spots->band_mask = new_mask;

    /* If band mask changed, expire spots on newly disabled bands */
    if (new_mask != old_mask && spots) {
        pic_spotlist_filter_bands(spots, new_mask);
    }
}
