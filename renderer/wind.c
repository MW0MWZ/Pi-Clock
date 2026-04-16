/*
 * wind.c - NOAA GFS wind data fetcher
 *
 * Runs a background thread that downloads pre-processed wind data
 * from the Pi-Clock data server (gh-pages). The data is a compact
 * binary with U and V wind components on a 1-degree global grid.
 *
 * Fetch schedule:
 *   - Immediately on thread start (boot)
 *   - Every 2 hours thereafter
 *
 * The data URL points to a gzipped binary file that the GitHub
 * Action updates every 6 hours from NOAA GFS.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "wind.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * Wind data URL — served from gh-pages, updated by GitHub Action.
 * The file is gzipped; wget handles decompression automatically
 * when Accept-Encoding is not set (it saves the raw .gz).
 * We download to a temp file and decompress manually.
 */
#define WIND_DATA_URL \
    "https://pi-clock.pistar.uk/data/gfs.bin.gz"

/* Fetch interval: 2 hours (7200 seconds) */
#define WIND_FETCH_INTERVAL 7200

/* Maximum raw (decompressed) file size.
 * 28-byte header + 2 * 360 * 181 * 4 = 521,308 bytes.
 * Allow a generous buffer. */
#define WIND_MAX_RAW (600 * 1024)

/* Local cache path for the downloaded file */
#define WIND_CACHE_PATH "/tmp/pi-clock-cache/gfs.bin.gz"
#define WIND_RAW_PATH   "/tmp/pi-clock-cache/gfs.bin"

/* Thread state */
static pthread_t wind_thread;
static volatile int wind_running = 0;
static pic_wind_t *cfg_data = NULL;

/*
 * pic_wind_init - Initialise the wind data buffer.
 */
void pic_wind_init(pic_wind_t *data)
{
    memset(data, 0, sizeof(*data));
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * parse_wind_binary - Parse the compact binary wind file.
 *
 * Reads the header, validates the magic and grid dimensions,
 * then copies the U and V float arrays into the data struct.
 *
 * Returns 1 on success, 0 on failure.
 */
/*
 * parse_gfs_binary - Parse the GFS2 multi-field binary format.
 *
 * GFS2 format:
 *   "GFS2" magic, nx, ny, timestamp, field_count, reserved
 *   For each field: 4-byte ID + nx*ny float32 array
 *
 * Also accepts legacy "WIND" format (2 fields, no ID tags).
 */
static int parse_gfs_binary(const char *path, pic_wind_t *data)
{
    FILE *f;
    unsigned char header[28];
    unsigned int nx, ny, field_count;
    unsigned long long ref_time;
    int expected;
    int is_gfs2;

    f = fopen(path, "rb");
    if (!f) {
        printf("wind: cannot open %s\n", path);
        return 0;
    }

    if (fread(header, 1, 28, f) != 28) {
        printf("wind: header too short\n");
        fclose(f);
        return 0;
    }

    /* Detect format */
    is_gfs2 = (memcmp(header, "GFS2", 4) == 0);
    if (!is_gfs2 && memcmp(header, "WIND", 4) != 0) {
        printf("wind: bad magic\n");
        fclose(f);
        return 0;
    }

    memcpy(&nx, header + 4, 4);
    memcpy(&ny, header + 8, 4);
    memcpy(&ref_time, header + 12, 8);

    if (is_gfs2) {
        memcpy(&field_count, header + 20, 4);
    } else {
        field_count = 2; /* Legacy: U and V only */
    }

    if (nx != PIC_WIND_NX || ny != PIC_WIND_NY) {
        printf("wind: unexpected grid %ux%u\n", nx, ny);
        fclose(f);
        return 0;
    }

    expected = PIC_WIND_NX * PIC_WIND_NY;

    pthread_mutex_lock(&data->mutex);
    data->has_cloud = 0;
    data->has_precip = 0;

    if (is_gfs2) {
        /* Read tagged fields */
        unsigned int fi;
        for (fi = 0; fi < field_count; fi++) {
            char tag[5] = {0};
            float *dest = NULL;

            if (fread(tag, 1, 4, f) != 4) break;

            /* Route each field to the right array */
            if (memcmp(tag, "UGRD", 4) == 0) dest = (float *)data->u;
            else if (memcmp(tag, "VGRD", 4) == 0) dest = (float *)data->v;
            else if (memcmp(tag, "TCDC", 4) == 0) dest = (float *)data->cloud;
            else if (memcmp(tag, "APCP", 4) == 0) dest = (float *)data->precip;
            else {
                /* Unknown field — skip it */
                if (fseek(f, (long)(expected * sizeof(float)), SEEK_CUR) != 0) {
                    printf("wind: seek failed skipping '%.4s'\n", tag);
                    break;
                }
                printf("wind: skipping unknown field '%.4s'\n", tag);
                continue;
            }

            if (fread(dest, sizeof(float), expected, f) != (size_t)expected) {
                printf("wind: field '%.4s' short read\n", tag);
                break;
            }

            /* Set flags only AFTER successful read to avoid
             * exposing partial data to renderers */
            if (memcmp(tag, "TCDC", 4) == 0) data->has_cloud = 1;
            if (memcmp(tag, "APCP", 4) == 0) data->has_precip = 1;
        }
    } else {
        /* Legacy WIND format: U then V, no tags */
        if (fread(data->u, sizeof(float), expected, f) != (size_t)expected ||
            fread(data->v, sizeof(float), expected, f) != (size_t)expected) {
            printf("wind: legacy format short read\n");
            pthread_mutex_unlock(&data->mutex);
            fclose(f);
            return 0;
        }
    }

    /* Note: time_t is 32-bit on armhf (wraps in 2038). ref_time is
     * only used as a cache invalidation key — if it wraps, the worst
     * case is a redundant cache redraw, not incorrect data. */
    data->ref_time = (time_t)ref_time;
    data->last_fetched = time(NULL);
    data->valid = 1;

    pthread_mutex_unlock(&data->mutex);
    fclose(f);

    printf("wind: loaded %dx%d grid, %u fields (ref %ld)%s\n",
           nx, ny, field_count, (long)ref_time,
           data->has_cloud ? " +cloud" : "");
    return 1;
}

/*
 * decompress_gz - Decompress a .gz file using the system gunzip.
 *
 * Returns 0 on success, -1 on failure.
 */
static int decompress_gz(const char *gz_path, const char *out_path)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: gunzip to stdout, redirect to output file */
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) _exit(1);
        dup2(fd, 1);
        close(fd);
        freopen("/dev/null", "w", stderr);
        execlp("gunzip", "gunzip", "-c", gz_path, NULL);
        _exit(127);
    }

    /* Wait for child */
    {
        int waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 30) {
            sleep(1);
            waited++;
        }
        if (waited >= 30) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
    }

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * wind_thread_func - Background thread for fetching wind data.
 */
static void *wind_thread_func(void *arg)
{
    (void)arg;

    /* Wait for network */
    if (!pic_wait_for_network("wind", &wind_running)) {
        return NULL;
    }

    /* Ensure cache directory exists — direct syscall, no fork needed */
    mkdir("/tmp/pi-clock-cache", 0755); /* EEXIST is expected and harmless */

    while (wind_running) {
        int ok;

        printf("wind: fetching from %s\n", WIND_DATA_URL);
        ok = pic_fetch_to_file(WIND_DATA_URL, WIND_CACHE_PATH, 60);

        if (ok == 0) {
            printf("wind: decompressing...\n");
            if (decompress_gz(WIND_CACHE_PATH, WIND_RAW_PATH) == 0) {
                parse_gfs_binary(WIND_RAW_PATH, cfg_data);
                /* Clean up tmpfs — data is now in memory. These files
                 * are ~1.6MB combined on tmpfs (which is RAM). */
                unlink(WIND_CACHE_PATH);
                unlink(WIND_RAW_PATH);
            } else {
                printf("wind: decompression failed\n");
            }
        } else {
            printf("wind: fetch failed, retrying in %ds\n",
                   WIND_FETCH_INTERVAL);
        }

        /* Sleep in 1-second chunks for responsive shutdown */
        {
            int s;
            for (s = 0; s < WIND_FETCH_INTERVAL && wind_running; s++)
                sleep(1);
        }
    }

    return NULL;
}

/*
 * pic_wind_start - Start the wind data fetch thread.
 */
int pic_wind_start(pic_wind_t *data)
{
    if (wind_running) return 0;

    cfg_data = data;
    wind_running = 1;

    if (pthread_create(&wind_thread, NULL, wind_thread_func, NULL) != 0) {
        wind_running = 0;
        fprintf(stderr, "wind: failed to create thread\n");
        return -1;
    }

    printf("wind: fetcher thread started (interval=%ds)\n",
           WIND_FETCH_INTERVAL);
    return 0;
}

/*
 * pic_wind_stop - Stop the background fetch thread.
 */
void pic_wind_stop(void)
{
    if (!wind_running) return;
    wind_running = 0;
    pthread_join(wind_thread, NULL);
    cfg_data = NULL;
    printf("wind: fetcher thread stopped\n");
}
