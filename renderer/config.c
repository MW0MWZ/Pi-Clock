/*
 * config.c - Global renderer configuration and projection helpers
 *
 * Provides the coordinate conversion functions used by all layers
 * to map between geographic coordinates (lat/lon) and pixel
 * positions. The key feature is center_lon support: the map can
 * be shifted so any longitude appears at the horizontal centre,
 * with correct wrapping at the edges.
 *
 * How centering works:
 *
 *   In a standard equirectangular map, longitude -180 is at the
 *   left edge and +180 is at the right edge, with 0 (Greenwich)
 *   in the middle. When center_lon is set to, say, 139.7 (Tokyo),
 *   we shift everything so that 139.7 maps to the middle pixel.
 *
 *   The math is: offset the longitude by (center_lon), then wrap
 *   into the range [-180, +180) using modular arithmetic. This
 *   moves the seam (where the map wraps around) to the opposite
 *   side of the globe from the center.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "config.h"
#include "layer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "fetch.h"

/* Global config instance — default center is Greenwich (0 degrees) */
pic_config_t pic_config = {
    .center_lon = 0.0
};

/*
 * wrap_lon - Wrap a longitude value into the range [-180, +180).
 *
 * Uses fmod() for the wrapping. This handles any input value,
 * including values that have been shifted far outside the normal
 * range by the centering offset.
 */
static double wrap_lon(double lon)
{
    lon = fmod(lon + 180.0, 360.0);
    if (lon < 0.0) {
        lon += 360.0;
    }
    return lon - 180.0;
}

/*
 * pic_lon_to_x - Convert longitude to X pixel coordinate.
 *
 * Apply the center offset, wrap to [-180, +180), then map
 * linearly to [0, width). The formula:
 *
 *   shifted_lon = wrap(lon - center_lon)
 *   x = (shifted_lon + 180) / 360 * width
 *
 * When center_lon is 0, this reduces to the standard equirectangular
 * projection: x = (lon + 180) / 360 * width.
 */
double pic_lon_to_x(double lon_deg, int width)
{
    double shifted = wrap_lon(lon_deg - pic_config.center_lon);
    return (shifted + 180.0) / 360.0 * width;
}

/*
 * pic_lat_to_y - Convert latitude to Y pixel coordinate.
 *
 * Standard equirectangular: Y increases downward on screen but
 * latitude increases upward, so we invert:
 *
 *   y = (90 - lat) / 180 * height
 *
 * This places the North Pole at Y=0 and South Pole at Y=height.
 * Not affected by center_lon (only horizontal centering).
 */
double pic_lat_to_y(double lat_deg, int height)
{
    return (90.0 - lat_deg) / 180.0 * height;
}

/*
 * pic_x_to_lon - Convert X pixel coordinate back to longitude.
 *
 * Inverse of pic_lon_to_x(). Undoes the centering offset:
 *
 *   shifted_lon = x / width * 360 - 180
 *   lon = wrap(shifted_lon + center_lon)
 *
 * Used by the daylight mask to find the real longitude of each pixel
 * so it can compute solar illumination correctly regardless of
 * how the map is centred.
 */
double pic_x_to_lon(double x, int width)
{
    double shifted = x / width * 360.0 - 180.0;
    return wrap_lon(shifted + pic_config.center_lon);
}

/*
 * pic_y_to_lat - Convert Y pixel coordinate back to latitude.
 *
 * Inverse of pic_lat_to_y():
 *   lat = 90 - y / height * 180
 */
double pic_y_to_lat(double y, int height)
{
    return 90.0 - y / height * 180.0;
}

/*
 * pic_load_layer_config - Read layer settings from config file.
 *
 * File format (one layer per line):
 *   layername=enabled,opacity
 *
 * Example:
 *   basemap=1,1.0
 *   daylight=1,1.0
 *   borders=1,0.8
 *   grid=1,0.5
 *   timezone=0,1.0
 *   hud=1,1.0
 */
#define LAYERS_CONF "/data/etc/pi-clock-layers.conf"

void pic_load_layer_config(pic_layer_stack_t *stack)
{
    FILE *f;
    char line[128];
    int i;

    f = fopen(LAYERS_CONF, "r");
    if (!f) {
        return; /* No config file — keep defaults */
    }

    printf("config: loading layer settings from %s\n", LAYERS_CONF);

    while (fgets(line, sizeof(line), f)) {
        char name[32];
        int enabled;
        double opacity;

        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        /* Parse: name=enabled,opacity */
        if (sscanf(line, "%31[^=]=%d,%lf", name, &enabled, &opacity) != 3) {
            continue;
        }

        /* Find matching layer in the stack and apply settings */
        for (i = 0; i < stack->count; i++) {
            if (strcmp(stack->layers[i].name, name) == 0) {
                int was_enabled = stack->layers[i].enabled;
                stack->layers[i].enabled = enabled;
                /* Clamp to valid Cairo alpha range */
                if (opacity < 0.0) opacity = 0.0;
                if (opacity > 1.0) opacity = 1.0;
                stack->layers[i].opacity = opacity;

                /* Free surface memory when a layer is disabled.
                 * On a 256MB Pi 1, each 1080p surface is ~8MB —
                 * freeing disabled layers saves significant RAM.
                 * The surface is re-allocated on-demand if the
                 * layer is later re-enabled. */
                if (was_enabled && !enabled) {
                    pic_layer_free_surface(&stack->layers[i]);
                }

                printf("config: layer '%s' enabled=%d opacity=%.2f\n",
                       name, enabled, opacity);
                break;
            }
        }
    }

    fclose(f);
}

/*
 * pic_load_renderer_conf - Parse the renderer config file into a struct.
 *
 * Reads /data/etc/pi-clock-renderer.conf line by line. Recognised keys:
 *   CALLSIGN, QTH_LAT, QTH_LON, CENTER_LON
 *
 * Keys not present in the file are left at their zero-initialised defaults.
 */
void pic_load_renderer_conf(pic_renderer_conf_t *conf)
{
    FILE *f;
    char line[256];

    memset(conf, 0, sizeof(*conf));

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        char *endp;
        double val;

        if (strncmp(line, "CALLSIGN=", 9) == 0) {
            sscanf(line + 9, "%15s", conf->callsign);
        } else if (strncmp(line, "QTH_LAT=", 8) == 0) {
            val = strtod(line + 8, &endp);
            if (endp != line + 8) conf->qth_lat = val;
        } else if (strncmp(line, "QTH_LON=", 8) == 0) {
            val = strtod(line + 8, &endp);
            if (endp != line + 8) conf->qth_lon = val;
        } else if (strncmp(line, "CENTER_LON=", 11) == 0) {
            val = strtod(line + 11, &endp);
            if (endp != line + 11) conf->center_lon = val;
        }
    }
    fclose(f);
}

/*
 * pic_wait_for_network - Poll for network connectivity at boot.
 *
 * Tries an HTTP fetch every 5 seconds for up to 2 minutes (24 attempts).
 * Uses the NOAA 10cm-flux endpoint as the test URL — it's lightweight
 * and we need it anyway for solar data.
 *
 * Returns 1 if network is ready, 0 if timed out or *running was cleared.
 */
int pic_wait_for_network(const char *label, const volatile int *running)
{
    int attempts = 0;

    while (*running && attempts < 24) { /* 24 × 5s = 2 minutes max */
        /* Test connectivity with a lightweight fetch */
        int ret = pic_fetch_to_file(
            "https://services.swpc.noaa.gov/products/summary/10cm-flux.json",
            "/dev/null", 5);
        if (ret == 0) {
            printf("%s: network ready\n", label);
            return 1;
        }
        if (attempts % 2 == 0)
            printf("%s: waiting for network (%d/24)...\n", label, attempts + 1);
        /* Sleep in 1s chunks so the stop flag is checked promptly.
         * Without this, shutdown can stall for up to 10s while a
         * thread is sleeping inside this function. */
        {
            int s;
            for (s = 0; s < 5 && *running; s++)
                sleep(1);
        }
        attempts++;
    }
    printf("%s: network wait timed out\n", label);
    return 0;
}
