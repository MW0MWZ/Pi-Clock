/*
 * layers_daylight.c - Daylight overlay layer (Blue Marble + solar mask)
 *
 * Paints Blue Marble satellite imagery only where it's currently
 * daytime, using a solar illumination alpha mask. Supports horizontal
 * centering via pic_config.center_lon.
 *
 * The approach:
 *   1. Compute solar illumination on a small grid (360x181, one
 *      value per degree) — very fast, ~65K trig calls
 *   2. Scale the grid up to display resolution using Cairo's
 *      bilinear filter — gives a silky smooth terminator with
 *      no visible stepping or blocky edges
 *   3. Paint Blue Marble through the scaled mask
 *
 * This is much faster AND smoother than the old per-pixel approach
 * with block fills. At 4K, the old method did ~32K trig calls with
 * visible 8px blocks at the terminator. The new method does ~65K
 * trig calls (slightly more) but the bilinear filter produces a
 * perfectly smooth gradient — no visible grid at any resolution.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "solar.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>
#include <string.h>

#include "math_utils.h"

/* Grid resolution for the illumination mask.
 * 360x181 = one sample per degree, matching the GFS weather grid.
 * At this resolution the terminator is computed to sub-degree
 * precision, and bilinear scaling smooths it perfectly. */
#define MASK_W 360
#define MASK_H 181

/*
 * pic_layer_render_daylight - Render the daylight overlay.
 *
 * Two-phase approach:
 *   Phase 1: Compute illumination on a small 360x181 grid
 *   Phase 2: Scale up to display resolution with bilinear filtering,
 *            then paint Blue Marble through the scaled mask
 */
void pic_layer_render_daylight(cairo_t *cr, int width, int height,
                               time_t now, void *user_data)
{
    cairo_surface_t *blue_marble = (cairo_surface_t *)user_data;
    cairo_surface_t *small_mask;
    cairo_surface_t *full_mask;
    unsigned char *mask_data;
    int mask_stride;
    pic_solar_position_t sun;
    int gx, gy;
    int src_w;
    double offset_x;

    if (!blue_marble) return;

    sun = pic_solar_position(now);

    /*
     * Phase 1: Build a small A8 illumination grid.
     * One byte per degree — 360x181 = ~65 KB.
     */
    small_mask = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                             MASK_W, MASK_H);
    if (cairo_surface_status(small_mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(small_mask);
        return;
    }

    cairo_surface_flush(small_mask);
    mask_data = cairo_image_surface_get_data(small_mask);
    mask_stride = cairo_image_surface_get_stride(small_mask);
    memset(mask_data, 0, mask_stride * MASK_H);

    for (gy = 0; gy < MASK_H; gy++) {
        double lat_deg = 90.0 - (double)gy;
        double lat_rad = lat_deg * DEG_TO_RAD;

        for (gx = 0; gx < MASK_W; gx++) {
            /* Map grid column to real longitude, accounting for
             * center_lon wrapping. Column 0 = left edge of display,
             * which corresponds to center_lon - 180. */
            double lon_deg = (double)gx - 180.0 + pic_config.center_lon;
            double lon_rad, illum;

            /* Wrap to -180..180 */
            if (lon_deg > 180.0) lon_deg -= 360.0;
            if (lon_deg < -180.0) lon_deg += 360.0;

            lon_rad = lon_deg * DEG_TO_RAD;
            illum = pic_solar_illumination(lat_rad, lon_rad, &sun);

            mask_data[gy * mask_stride + gx] =
                (unsigned char)(illum * 255.0);
        }
    }

    cairo_surface_mark_dirty(small_mask);

    /*
     * Phase 2: Scale the small mask up to display resolution using
     * bilinear filtering, producing a smooth terminator gradient.
     */
    full_mask = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                            width, height);
    if (cairo_surface_status(full_mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(small_mask);
        cairo_surface_destroy(full_mask);
        return;
    }

    {
        cairo_t *mc = cairo_create(full_mask);

        cairo_scale(mc,
                    (double)width / MASK_W,
                    (double)height / MASK_H);
        cairo_set_source_surface(mc, small_mask, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(mc),
                                 CAIRO_FILTER_BILINEAR);
        cairo_paint(mc);
        cairo_destroy(mc);
    }

    cairo_surface_destroy(small_mask);

    /*
     * Phase 3: Paint Blue Marble through the scaled mask.
     *
     * The mask is already in display coordinates (no center_lon
     * offset needed — we computed it with the offset baked in).
     * But the Blue Marble image needs wrapping to match center_lon.
     */
    src_w = cairo_image_surface_get_width(blue_marble);
    offset_x = (pic_config.center_lon / 360.0) * src_w;

    {
        cairo_pattern_t *pattern;

        /* Repeating Blue Marble pattern with wrapping offset */
        pattern = cairo_pattern_create_for_surface(blue_marble);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

        {
            cairo_matrix_t matrix;
            cairo_matrix_init_translate(&matrix, offset_x, 0);
            cairo_pattern_set_matrix(pattern, &matrix);
        }

        cairo_set_source(cr, pattern);
        cairo_mask_surface(cr, full_mask, 0, 0);
        cairo_pattern_destroy(pattern);
    }

    cairo_surface_destroy(full_mask);
}
