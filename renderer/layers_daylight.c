/*
 * layers_daylight.c - Daylight overlay layer (Blue Marble + solar mask)
 *
 * Paints Blue Marble satellite imagery only where it's currently
 * daytime, using a solar illumination alpha mask.
 *
 * Two-phase approach (preserved from the pre-zoom design):
 *   Phase 1: Compute illumination on a small 360x181 grid covering
 *            the whole globe at one sample per degree. Independent
 *            of the viewport — cheap to compute, easy to cache in
 *            future if needed.
 *   Phase 2: Scale the small mask up to display resolution through
 *            the current viewport, producing a silky-smooth
 *            terminator with bilinear filtering.
 *   Phase 3: Paint the Blue Marble image through the scaled mask,
 *            again via the viewport transform. Longitude wrap is
 *            handled by CAIRO_EXTEND_REPEAT in pic_paint_viewport.
 *
 * Compared to the pre-zoom code, the big change is that phases 2
 * and 3 now run through pic_paint_viewport() rather than a simple
 * cairo_scale(width/MASK_W, height/MASK_H) + a center_lon-only
 * offset. Phase 1 is simpler because we no longer bake the
 * center_lon shift into the grid — the viewport transform does it.
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
 * Column 0 maps to lon=-180°, column 359 to lon=+179°. Row 0 maps
 * to lat=+90°, row 180 to lat=-90°. */
#define MASK_W 360
#define MASK_H 181

/*
 * pic_layer_render_daylight - Render the daylight overlay.
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

    if (!blue_marble) return;

    sun = pic_solar_position(now);

    /*
     * Phase 1: Build a small A8 illumination grid covering the whole
     * globe at one sample per degree. Column gx encodes lon directly
     * (-180 + gx degrees), row gy encodes lat directly (90 - gy).
     * No viewport offset applied here — the transform in phase 2
     * handles it.
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
            double lon_deg = (double)gx - 180.0;
            double lon_rad = lon_deg * DEG_TO_RAD;
            double illum = pic_solar_illumination(lat_rad, lon_rad, &sun);

            mask_data[gy * mask_stride + gx] =
                (unsigned char)(illum * 255.0);
        }
    }

    cairo_surface_mark_dirty(small_mask);

    /*
     * Phase 2: Scale the small mask up to display resolution through
     * the current viewport. pic_paint_viewport handles both pan (centre
     * shift) and zoom (span scaling) with bilinear filtering, producing
     * a smooth terminator at any zoom level.
     *
     * Note: both small_mask and full_mask are CAIRO_FORMAT_A8.
     * pic_paint_viewport works for A8 sources because it uses a
     * generic pattern paint which preserves the alpha channel into
     * the A8 destination. If we ever switch full_mask to ARGB32
     * (for e.g. coloured twilight), this step would need a bespoke
     * A8-to-ARGB path since CAIRO_OPERATOR_OVER on an A8 pattern
     * source would write zeros for the colour channels.
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
        pic_paint_viewport(mc, small_mask, width, height);
        cairo_destroy(mc);
    }

    cairo_surface_destroy(small_mask);

    /*
     * Phase 3: Paint Blue Marble through the scaled mask.
     *
     * The Blue Marble pattern is built via pic_viewport_pattern so
     * zoom and pan are applied identically to the basemap (and
     * phase 2 above). cairo_mask_surface then uses full_mask's
     * alpha as a stencil over the Blue Marble source.
     */
    {
        cairo_pattern_t *pattern =
            pic_viewport_pattern(blue_marble, width, height);
        if (pattern) {
            cairo_set_source(cr, pattern);
            cairo_mask_surface(cr, full_mask, 0, 0);
            cairo_pattern_destroy(pattern);
        }
    }

    cairo_surface_destroy(full_mask);
}
