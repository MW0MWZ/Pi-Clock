/*
 * layers_precip.c - Precipitation overlay layer (Rain Radar)
 *
 * Renders accumulated precipitation from NOAA GFS using the
 * standard weather radar colour scheme. Uses bilinear-interpolated
 * scaling for smooth, non-blocky rendering.
 *
 * Approach: render data to a tiny 360x181 surface (one pixel per
 * grid cell), then scale up to display resolution with Cairo's
 * bilinear filter. This gives smooth interpolation between cells
 * and natural fade at rain boundaries — no visible grid lines.
 *
 * Colour scheme (standard NWS weather radar):
 *   < 0.5mm:  transparent (trace)
 *   0.5-2mm:  green (light rain / drizzle)
 *   2-5mm:    yellow (moderate rain)
 *   5-10mm:   orange (heavy rain)
 *   10-20mm:  red (very heavy / thunderstorm)
 *   20-40mm:  magenta (severe / torrential)
 *   40mm+:    purple (extreme)
 *
 * Data source: NOAA GFS APCP field (public domain).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "wind.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Surface cache ───────────────────────────────────────────── */

static cairo_surface_t *precip_cache = NULL;
static int precip_cache_w = 0, precip_cache_h = 0;
static time_t precip_cache_ref = 0;

/*
 * radar_colour - Map precipitation amount to ARGB pixel value.
 *
 * Returns a pre-multiplied ARGB32 pixel for direct surface writing.
 */
static unsigned int radar_pixel(double mm)
{
    double r, g, b, a;
    unsigned int ir, ig, ib, ia;

    if (mm < 0.5) return 0;

    /* Alpha: starts subtle, increases with intensity */
    {
        double intensity = mm / 40.0;
        if (intensity > 1.0) intensity = 1.0;
        a = 0.25 + intensity * 0.45;
    }

    if (mm < 2.0) {
        double t = (mm - 0.5) / 1.5;
        r = 0.1; g = 0.5 + 0.3 * t; b = 0.1;
    } else if (mm < 5.0) {
        double t = (mm - 2.0) / 3.0;
        r = 0.1 + 0.8 * t; g = 0.8 - 0.1 * t; b = 0.1;
    } else if (mm < 10.0) {
        double t = (mm - 5.0) / 5.0;
        r = 0.9 + 0.1 * t; g = 0.7 - 0.4 * t; b = 0.0;
    } else if (mm < 20.0) {
        double t = (mm - 10.0) / 10.0;
        r = 1.0; g = 0.3 - 0.3 * t; b = 0.0;
    } else if (mm < 40.0) {
        double t = (mm - 20.0) / 20.0;
        r = 1.0; g = 0.0; b = 0.3 + 0.5 * t;
    } else {
        r = 0.7; g = 0.0; b = 0.8;
    }

    /* Pre-multiply alpha for ARGB32 surface format */
    ia = (unsigned int)(a * 255);
    ir = (unsigned int)(r * a * 255);
    ig = (unsigned int)(g * a * 255);
    ib = (unsigned int)(b * a * 255);

    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

/*
 * pic_layer_render_precip - Render precipitation radar overlay.
 *
 * Writes data to a tiny 360x181 surface, then scales up with
 * bilinear filtering for smooth, non-blocky rendering.
 */
void pic_layer_render_precip(cairo_t *cr, int width, int height,
                             time_t now, void *user_data)
{
    pic_wind_t *data = (pic_wind_t *)user_data;
    time_t ref;
    int need_redraw = 0;

    (void)now;
    if (!data) return;

    pthread_mutex_lock(&data->mutex);
    if (!data->valid || !data->has_precip) {
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    ref = data->ref_time;
    pthread_mutex_unlock(&data->mutex);

    if (!precip_cache ||
        precip_cache_w != width || precip_cache_h != height) {
        if (precip_cache) cairo_surface_destroy(precip_cache);
        precip_cache = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(precip_cache) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(precip_cache);
            precip_cache = NULL;
            return;
        }
        precip_cache_w = width;
        precip_cache_h = height;
        need_redraw = 1;
    }

    if (ref != precip_cache_ref) need_redraw = 1;

    if (!need_redraw) {
        cairo_set_source_surface(cr, precip_cache, 0, 0);
        cairo_paint(cr);
        return;
    }

    {
        /* Create a tiny surface at grid resolution */
        cairo_surface_t *grid_surf;
        unsigned int *pixels;
        int stride, gy, gx;
        int center_off;
        cairo_t *cc;

        grid_surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, PIC_WIND_NX, PIC_WIND_NY);
        if (cairo_surface_status(grid_surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(grid_surf);
            return;
        }

        /* Write pixel data directly — much faster than 65K cairo calls */
        cairo_surface_flush(grid_surf);
        pixels = (unsigned int *)cairo_image_surface_get_data(grid_surf);
        stride = cairo_image_surface_get_stride(grid_surf) / 4;

        /* Center longitude offset in grid columns */
        center_off = (int)(pic_config.center_lon);
        if (center_off < 0) center_off += 360;

        pthread_mutex_lock(&data->mutex);

        for (gy = 0; gy < PIC_WIND_NY; gy++) {
            for (gx = 0; gx < PIC_WIND_NX; gx++) {
                int src_col = (gx + center_off) % PIC_WIND_NX;
                pixels[gy * stride + gx] =
                    radar_pixel(data->precip[gy][src_col]);
            }
        }

        pthread_mutex_unlock(&data->mutex);
        cairo_surface_mark_dirty(grid_surf);

        /* Scale up to display resolution with bilinear filtering */
        cc = cairo_create(precip_cache);
        cairo_set_operator(cc, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cc);
        cairo_set_operator(cc, CAIRO_OPERATOR_OVER);

        cairo_scale(cc,
                    (double)width / PIC_WIND_NX,
                    (double)height / PIC_WIND_NY);
        cairo_set_source_surface(cc, grid_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cc),
                                 CAIRO_FILTER_BILINEAR);
        cairo_paint(cc);

        cairo_destroy(cc);
        cairo_surface_destroy(grid_surf);
        precip_cache_ref = ref;
        printf("precip: overlay cached (bilinear)\n");
    }

    cairo_set_source_surface(cr, precip_cache, 0, 0);
    cairo_paint(cr);
}

void pic_precip_cleanup(void)
{
    if (precip_cache) {
        cairo_surface_destroy(precip_cache);
        precip_cache = NULL;
        precip_cache_w = 0;
        precip_cache_h = 0;
    }
}
