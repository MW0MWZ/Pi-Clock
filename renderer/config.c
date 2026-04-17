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

/* Global config instance — default center is Greenwich, no zoom.
 *
 * The viewport fields are initialised by pic_config_recompute_viewport
 * at startup (after the renderer conf file is parsed) — the defaults
 * below are the zoom=0 values so the renderer works even if that call
 * is skipped. */
pic_config_t pic_config = {
    .center_lon      = 0.0,
    .map_zoom        = 0.0,
    .qth_lat         = 0.0,
    .qth_lon         = 0.0,
    .view_center_lat = 0.0,
    .view_center_lon = 0.0,
    .view_span_lat   = 180.0,
    .view_span_lon   = 360.0,
    .viewport_gen    = 0
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
 * pic_zoom_factor - Linear interp from 1.0 at map_zoom=0 to
 * PIC_ZOOM_MAX at map_zoom=1. Clamped so bad config doesn't
 * produce divide-by-zero or negative spans.
 */
double pic_zoom_factor(void)
{
    double f = pic_config.map_zoom;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return 1.0 + f * (PIC_ZOOM_MAX - 1.0);
}

/*
 * pic_wrap_threshold_px - Pixel distance corresponding to 180° of
 * angular separation at the current viewport. See config.h.
 */
double pic_wrap_threshold_px(int width)
{
    /* (180 / view_span_lon) * width. Guard against zero span. */
    double span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    return (180.0 / span) * (double)width;
}

/*
 * pic_config_recompute_viewport - Derive view_* fields from
 * center_lon, map_zoom, qth_lat and qth_lon.
 *
 * Longitude centering rule (user-specified):
 *   view_center_lon stays at center_lon. The "pan toward QTH"
 *   behaviour is latitude-only; horizontal framing is preserved
 *   so users who configured, say, a Pacific-centred map keep it.
 *
 * Latitude centering rule:
 *   view_center_lat *aims* at QTH latitude but is clamped so the
 *   viewport never extends past the poles. That produces a smooth
 *   progression: at zoom=0 (full world) the clamp forces the
 *   centre to 0°; as the viewport shrinks the clamp relaxes and
 *   the centre walks toward QTH; once the viewport is small enough
 *   to fit QTH entirely (span/2 ≤ 90 - |qth_lat|) the clamp lets
 *   go and QTH sits dead centre. An earlier design used a linear
 *   blend `f * qth_lat` which left the QTH near the top edge even
 *   at 50% zoom — this clamp-driven version centres QTH much
 *   sooner while still starting from a world view.
 *
 * QTH "unset" handling:
 *   The rest of the renderer treats QTH at exactly 0°/0° (Gulf of
 *   Guinea) as "unset" — see layers_qth.c. If qth is (0, 0) we
 *   leave view_center_lat at 0 regardless of zoom, so an unset QTH
 *   produces a normal equator-centred zoom rather than pivoting
 *   on an unintended point.
 */
void pic_config_recompute_viewport(void)
{
    double f = pic_config.map_zoom;
    double z, half_lat;

    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;

    z = 1.0 + f * (PIC_ZOOM_MAX - 1.0);

    pic_config.view_span_lon = 360.0 / z;
    pic_config.view_span_lat = 180.0 / z;

    pic_config.view_center_lon = pic_config.center_lon;

    /* Aim for QTH, let the pole clamp pull us back as needed. */
    if (pic_config.qth_lat == 0.0 && pic_config.qth_lon == 0.0) {
        pic_config.view_center_lat = 0.0;
    } else {
        pic_config.view_center_lat = pic_config.qth_lat;
    }

    /* Clamp so the view never exceeds either pole.
     * Example: QTH at 52°N with zoom=1 (full world) → span 180° →
     * hi=0, lo=0, view_center_lat=0 (world view). As zoom grows,
     * hi grows with it until it >= qth_lat and the clamp releases. */
    half_lat = pic_config.view_span_lat / 2.0;
    if (pic_config.view_center_lat + half_lat > 90.0) {
        pic_config.view_center_lat = 90.0 - half_lat;
    }
    if (pic_config.view_center_lat - half_lat < -90.0) {
        pic_config.view_center_lat = -90.0 + half_lat;
    }

    pic_config.viewport_gen++;
}

/*
 * pic_lon_to_x - Convert longitude to X pixel coordinate.
 *
 * Uses the viewport: delta (wrapped to [-180, +180)) from the
 * view centre is scaled by the viewport's longitude span. At
 * zoom=0 with center_lon=0 this reduces to the standard
 * equirectangular formula x = (lon + 180) / 360 * width.
 *
 * With zoom, points outside the viewport project to pixel values
 * outside [0, width) — Cairo clips them naturally. Layers that
 * draw polylines should use pic_wrap_threshold_px() to detect
 * seam crossings rather than assuming width/2.
 */
double pic_lon_to_x(double lon_deg, int width)
{
    double delta = wrap_lon(lon_deg - pic_config.view_center_lon);
    return (delta / pic_config.view_span_lon + 0.5) * (double)width;
}

/*
 * pic_lat_to_y - Convert latitude to Y pixel coordinate.
 *
 * Y increases downward on screen; latitude increases upward,
 * so we flip. Uses the viewport's latitude span, so zoomed-in
 * latitudes outside the view project to y outside [0, height).
 *
 * At zoom=0 (view_center_lat=0, view_span_lat=180) this reduces
 * to y = (90 - lat) / 180 * height.
 */
double pic_lat_to_y(double lat_deg, int height)
{
    return ((pic_config.view_center_lat - lat_deg)
            / pic_config.view_span_lat + 0.5) * (double)height;
}

/*
 * pic_x_to_lon - Inverse of pic_lon_to_x. Returns longitude
 * wrapped to [-180, +180).
 */
double pic_x_to_lon(double x, int width)
{
    double delta = ((double)x / (double)width - 0.5)
                   * pic_config.view_span_lon;
    return wrap_lon(pic_config.view_center_lon + delta);
}

/*
 * pic_y_to_lat - Inverse of pic_lat_to_y. Not clamped to
 * [-90, +90] — at extreme off-screen y the result can exceed
 * the poles; callers doing solar maths should clamp if it
 * matters to them.
 */
double pic_y_to_lat(double y, int height)
{
    return pic_config.view_center_lat
           - ((double)y / (double)height - 0.5)
             * pic_config.view_span_lat;
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
 *   CALLSIGN, QTH_LAT, QTH_LON, CENTER_LON, MAP_ZOOM
 *
 * MAP_ZOOM is stored on disk as a 0-100 percentage (dashboard slider
 * units) and converted here to a 0.0-1.0 fraction, clamped for safety.
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

        /* All numeric values go through isfinite() so a corrupt
         * config file with "nan"/"inf" can't poison the viewport
         * math (nan compares false against any bound and silently
         * propagates through arithmetic). */
        if (strncmp(line, "CALLSIGN=", 9) == 0) {
            sscanf(line + 9, "%15s", conf->callsign);
        } else if (strncmp(line, "QTH_LAT=", 8) == 0) {
            val = strtod(line + 8, &endp);
            if (endp != line + 8 && isfinite(val)) conf->qth_lat = val;
        } else if (strncmp(line, "QTH_LON=", 8) == 0) {
            val = strtod(line + 8, &endp);
            if (endp != line + 8 && isfinite(val)) conf->qth_lon = val;
        } else if (strncmp(line, "CENTER_LON=", 11) == 0) {
            val = strtod(line + 11, &endp);
            if (endp != line + 11 && isfinite(val)) conf->center_lon = val;
        } else if (strncmp(line, "MAP_ZOOM=", 9) == 0) {
            val = strtod(line + 9, &endp);
            if (endp != line + 9 && isfinite(val)) {
                /* Dashboard writes 0..100; clamp and scale to 0..1 */
                if (val < 0.0)   val = 0.0;
                if (val > 100.0) val = 100.0;
                conf->map_zoom = val / 100.0;
            }
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
