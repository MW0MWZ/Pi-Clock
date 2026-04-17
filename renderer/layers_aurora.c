/*
 * layers_aurora.c - Aurora oval overlay layer
 *
 * Renders the aurora borealis/australis as a translucent glow
 * based on NOAA SWPC OVATION probability data. Uses bilinear
 * scaling for smooth edges and realistic colour grading.
 *
 * Colour scheme — realistic aurora colours, distinct from rain
 * radar (which uses green→yellow→orange→red warm spectrum):
 *
 *   Low probability (5-20%):    dim teal/cyan glow
 *   Medium (20-50%):            bright cyan-green
 *   High (50-80%):              bright green shifting to white
 *   Very high (80%+):           intense white with blue-violet tinge
 *
 * Real aurora is dominated by:
 *   - 557.7nm green (oxygen) — the classic aurora green
 *   - 630.0nm red (oxygen at high altitude) — upper fringe
 *   - 427.8nm blue-violet (nitrogen) — lower edge/curtains
 *
 * We use the cool spectrum (teal → green → blue-white) to avoid
 * conflict with the warm rain radar palette.
 *
 * Data source: NOAA SWPC OVATION aurora nowcast (public domain).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "aurora.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static cairo_surface_t *aurora_cache = NULL;
static int aurora_cache_w = 0, aurora_cache_h = 0;
static time_t aurora_cache_time = 0;
/* Viewport generation baked into the cached surface. */
static unsigned int aurora_cache_viewport_gen = 0;

/*
 * aurora_pixel - Map aurora probability to a pre-multiplied ARGB pixel.
 *
 * Cool spectrum to avoid rain radar colours:
 *   Low:    dim teal       (R=0.1, G=0.6, B=0.5)
 *   Medium: bright cyan    (R=0.2, G=0.9, B=0.7)
 *   High:   green-white    (R=0.5, G=1.0, B=0.8)
 *   Max:    blue-white     (R=0.7, G=0.9, B=1.0)
 */
static unsigned int aurora_pixel(double prob)
{
    double r, g, b, a;
    unsigned int ia, ir, ig, ib;

    if (prob < 2.0) return 0;

    double frac = (prob - 2.0) / 80.0; /* 0-1, saturates at 82% */
    if (frac > 1.0) frac = 1.0;

    /* Colour gradient: teal → cyan → green-white → blue-white */
    if (frac < 0.3) {
        double t = frac / 0.3;
        r = 0.1 + 0.1 * t;
        g = 0.5 + 0.4 * t;
        b = 0.4 + 0.3 * t;
    } else if (frac < 0.7) {
        double t = (frac - 0.3) / 0.4;
        r = 0.2 + 0.3 * t;
        g = 0.9 + 0.1 * t;
        b = 0.7 + 0.1 * t;
    } else {
        double t = (frac - 0.7) / 0.3;
        r = 0.5 + 0.2 * t;
        g = 1.0 - 0.1 * t;
        b = 0.8 + 0.2 * t;
    }

    /* Opacity: minimum 0.08 so the oval is visible even during
     * quiet conditions (Kp 0-1, prob 5-15%). Max 0.65. */
    a = 0.08 + frac * 0.57;

    /* Pre-multiply alpha */
    ia = (unsigned int)(a * 255);
    ir = (unsigned int)(r * a * 255);
    ig = (unsigned int)(g * a * 255);
    ib = (unsigned int)(b * a * 255);

    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

void pic_layer_render_aurora(cairo_t *cr, int width, int height,
                             time_t now, void *user_data)
{
    pic_aurora_t *data = (pic_aurora_t *)user_data;
    time_t fetched;
    int need_redraw = 0;

    (void)now;
    if (!data) return;

    pthread_mutex_lock(&data->mutex);
    if (!data->valid) {
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    fetched = data->last_fetched;
    pthread_mutex_unlock(&data->mutex);

    if (!aurora_cache ||
        aurora_cache_w != width || aurora_cache_h != height) {
        if (aurora_cache) cairo_surface_destroy(aurora_cache);
        aurora_cache = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(aurora_cache) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(aurora_cache);
            aurora_cache = NULL;
            return;
        }
        aurora_cache_w = width;
        aurora_cache_h = height;
        need_redraw = 1;
    }

    if (fetched != aurora_cache_time) need_redraw = 1;
    if (pic_config.viewport_gen != aurora_cache_viewport_gen) need_redraw = 1;

    if (!need_redraw) {
        cairo_set_source_surface(cr, aurora_cache, 0, 0);
        cairo_paint(cr);
        return;
    }

    {
        cairo_surface_t *grid_surf;
        unsigned int *pixels;
        int stride, gy, gx;
        cairo_t *cc;

        grid_surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, PIC_AURORA_NX, PIC_AURORA_NY);
        if (cairo_surface_status(grid_surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(grid_surf);
            return;
        }

        cairo_surface_flush(grid_surf);
        pixels = (unsigned int *)cairo_image_surface_get_data(grid_surf);
        stride = cairo_image_surface_get_stride(grid_surf) / 4;

        /* Raw lat/lon grid: column 0 = lon -180°, row 0 = lat +90°.
         * pic_paint_viewport applies the pan + zoom transform below. */
        pthread_mutex_lock(&data->mutex);

        for (gy = 0; gy < PIC_AURORA_NY; gy++) {
            for (gx = 0; gx < PIC_AURORA_NX; gx++) {
                pixels[gy * stride + gx] =
                    aurora_pixel(data->prob[gy][gx]);
            }
        }

        pthread_mutex_unlock(&data->mutex);
        cairo_surface_mark_dirty(grid_surf);

        /* Paint grid through the current viewport — bilinear filter
         * in pic_paint_viewport keeps aurora edges smooth at any zoom. */
        cc = cairo_create(aurora_cache);
        cairo_set_operator(cc, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cc);
        cairo_set_operator(cc, CAIRO_OPERATOR_OVER);

        pic_paint_viewport(cc, grid_surf, width, height);

        cairo_destroy(cc);
        cairo_surface_destroy(grid_surf);
        aurora_cache_time = fetched;
        aurora_cache_viewport_gen = pic_config.viewport_gen;
        printf("aurora: overlay cached (viewport gen=%u)\n",
               aurora_cache_viewport_gen);
    }

    cairo_set_source_surface(cr, aurora_cache, 0, 0);
    cairo_paint(cr);
}

void pic_aurora_cleanup(void)
{
    if (aurora_cache) {
        cairo_surface_destroy(aurora_cache);
        aurora_cache = NULL;
        aurora_cache_w = 0;
        aurora_cache_h = 0;
    }
}
