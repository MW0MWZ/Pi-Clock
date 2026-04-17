/*
 * layers_cloud.c - Cloud cover overlay layer
 *
 * Renders total cloud cover from NOAA GFS as a semi-transparent
 * white haze. Uses bilinear-interpolated scaling for smooth edges.
 *
 * Approach: render to a tiny 360x181 surface, scale up with
 * bilinear filtering. Cloud edges blend naturally to transparent.
 *
 * Data source: NOAA GFS TCDC field (public domain).
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

static cairo_surface_t *cloud_cache = NULL;
static int cloud_cache_w = 0, cloud_cache_h = 0;
static time_t cloud_cache_ref = 0;
/* Viewport generation baked into the cached surface. Set by
 * pic_config_recompute_viewport; when it differs we redraw. */
static unsigned int cloud_cache_viewport_gen = 0;

void pic_layer_render_cloud(cairo_t *cr, int width, int height,
                            time_t now, void *user_data)
{
    pic_wind_t *data = (pic_wind_t *)user_data;
    time_t ref;
    int need_redraw = 0;

    (void)now;
    if (!data) return;

    pthread_mutex_lock(&data->mutex);
    if (!data->valid || !data->has_cloud) {
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    ref = data->ref_time;
    pthread_mutex_unlock(&data->mutex);

    if (!cloud_cache ||
        cloud_cache_w != width || cloud_cache_h != height) {
        if (cloud_cache) cairo_surface_destroy(cloud_cache);
        cloud_cache = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(cloud_cache) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(cloud_cache);
            cloud_cache = NULL;
            return;
        }
        cloud_cache_w = width;
        cloud_cache_h = height;
        need_redraw = 1;
    }

    if (ref != cloud_cache_ref) need_redraw = 1;
    if (pic_config.viewport_gen != cloud_cache_viewport_gen) need_redraw = 1;

    if (!need_redraw) {
        cairo_set_source_surface(cr, cloud_cache, 0, 0);
        cairo_paint(cr);
        return;
    }

    {
        cairo_surface_t *grid_surf;
        unsigned int *pixels;
        int stride, gy, gx;
        cairo_t *cc;

        grid_surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, PIC_WIND_NX, PIC_WIND_NY);
        if (cairo_surface_status(grid_surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(grid_surf);
            return;
        }

        cairo_surface_flush(grid_surf);
        pixels = (unsigned int *)cairo_image_surface_get_data(grid_surf);
        stride = cairo_image_surface_get_stride(grid_surf) / 4;

        /* Write the grid in raw lat/lon order: column 0 = lon -180°,
         * column PIC_WIND_NX-1 = lon ~+179°. pic_paint_viewport
         * applies the viewport centre/zoom transform when painting
         * onto the cache surface below, so there is no need to
         * pre-shift columns here. */
        pthread_mutex_lock(&data->mutex);

        for (gy = 0; gy < PIC_WIND_NY; gy++) {
            for (gx = 0; gx < PIC_WIND_NX; gx++) {
                double cover = data->cloud[gy][gx];
                unsigned int ia, ic;

                if (cover < 5.0) {
                    pixels[gy * stride + gx] = 0;
                    continue;
                }

                /* Squared curve: thin cloud subtle, thick dense.
                 * Colour: slightly blue-tinted white. Max alpha 0.55. */
                {
                    double norm = cover / 100.0;
                    double a = norm * norm * 0.55;
                    ia = (unsigned int)(a * 255);
                    ic = (unsigned int)(0.9 * a * 255); /* RGB premultiplied */
                }

                pixels[gy * stride + gx] =
                    (ia << 24) | (ic << 16) | (ic << 8) | ic;
            }
        }

        pthread_mutex_unlock(&data->mutex);
        cairo_surface_mark_dirty(grid_surf);

        cc = cairo_create(cloud_cache);
        cairo_set_operator(cc, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cc);
        cairo_set_operator(cc, CAIRO_OPERATOR_OVER);

        pic_paint_viewport(cc, grid_surf, width, height);

        cairo_destroy(cc);
        cairo_surface_destroy(grid_surf);
        cloud_cache_ref = ref;
        cloud_cache_viewport_gen = pic_config.viewport_gen;
        printf("cloud: overlay cached (viewport gen=%u)\n",
               cloud_cache_viewport_gen);
    }

    cairo_set_source_surface(cr, cloud_cache, 0, 0);
    cairo_paint(cr);
}

void pic_cloud_cleanup(void)
{
    if (cloud_cache) {
        cairo_surface_destroy(cloud_cache);
        cloud_cache = NULL;
        cloud_cache_w = 0;
        cloud_cache_h = 0;
    }
}
