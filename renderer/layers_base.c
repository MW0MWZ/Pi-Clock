/*
 * layers_base.c - Base map layer renderer (Black Marble) + shared
 *                 viewport paint helper.
 *
 * Responsible for drawing the NASA Black Marble (Earth at Night) as
 * the bottom layer of the compositing stack. The image is a full
 * 360°×180° equirectangular texture; we paint a sub-region of it
 * matching the current viewport (pic_config.view_*).
 *
 * Viewport paint approach:
 *
 *   Cairo affine matrix maps destination pixels (0..width, 0..height)
 *   to source pixels. Deriving that matrix from the viewport gives
 *   both pan (center shift) and zoom (span scaling) in one Cairo
 *   paint — no per-pixel loop, no manual tile repetition.
 *
 *   Source pixel for destination pixel (px, py):
 *     lon = view_center_lon + (px/width  - 0.5) * view_span_lon
 *     lat = view_center_lat - (py/height - 0.5) * view_span_lat
 *     sx  = (lon + 180) * src_w / 360
 *     sy  = (90 - lat)  * src_h / 180
 *
 *   Rearranged, sx and sy are linear in px and py, which matches
 *   Cairo's cairo_matrix_t (6-element affine). CAIRO_EXTEND_REPEAT
 *   handles longitude wrap naturally when the viewport straddles the
 *   seam opposite center_lon.
 *
 * Latitude is clamped in pic_config_recompute_viewport so sy always
 * stays inside [0, src_h], meaning REPEAT mode doesn't falsely tile
 * poles. (Lon REPEAT is intentional.)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"

/*
 * pic_viewport_pattern - Build the Cairo pattern + matrix used by
 * all viewport paints. Factored out so pic_paint_viewport and the
 * daylight layer (which needs cairo_mask_surface instead of
 * cairo_paint) share one copy of the math.
 *
 * Thread-safety note: this reads pic_config view_* and span_*
 * fields without a lock. That is safe because the entire render
 * loop and the SIGHUP handler that writes those fields both run
 * on the main thread (see display.c - g_reload is checked at the
 * top of the loop, before pic_layer_stack_update). Background
 * fetch threads never touch pic_config; they only write into
 * their own data structs which the render functions read under
 * per-struct mutexes.
 */
cairo_pattern_t *pic_viewport_pattern(cairo_surface_t *src,
                                      int width, int height)
{
    cairo_pattern_t *pattern;
    cairo_matrix_t m;
    int src_w, src_h;
    double a, b, c, d;
    double span_lon, span_lat;

    if (!src) return NULL;

    src_w = cairo_image_surface_get_width(src);
    src_h = cairo_image_surface_get_height(src);
    if (src_w <= 0 || src_h <= 0) return NULL;

    span_lon = pic_config.view_span_lon;
    span_lat = pic_config.view_span_lat;
    /* Safety: a zero span would make the matrix singular. At
     * recompute_viewport these are clamped, but guard anyway. */
    if (span_lon < 0.0001) span_lon = 360.0;
    if (span_lat < 0.0001) span_lat = 180.0;

    /* sx = a*px + b ; sy = c*py + d  - see file header for derivation */
    a = span_lon * (double)src_w / (360.0 * (double)width);
    b = (pic_config.view_center_lon - span_lon / 2.0 + 180.0)
        * (double)src_w / 360.0;
    c = span_lat * (double)src_h / (180.0 * (double)height);
    d = (90.0 - pic_config.view_center_lat - span_lat / 2.0)
        * (double)src_h / 180.0;

    pattern = cairo_pattern_create_for_surface(src);
    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(pattern);
        return NULL;
    }

    /* REPEAT: longitude wraps around the globe seamlessly. Latitude
     * stays in range by viewport clamping, so REPEAT doesn't stack
     * poles. BILINEAR: smooth scaling at any zoom level. */
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BILINEAR);

    /* cairo_matrix_init takes row-major components: sx = xx*x + xy*y + x0,
     * sy = yx*x + yy*y + y0. Our mapping has no cross-axis terms. */
    cairo_matrix_init(&m, a, 0.0, 0.0, c, b, d);
    cairo_pattern_set_matrix(pattern, &m);

    return pattern;
}

/*
 * pic_paint_viewport - Shared by basemap, cloud, precip, aurora
 * (and anywhere a full-globe surface should be drawn through the
 * viewport). See layer.h for the contract.
 */
void pic_paint_viewport(cairo_t *cr, cairo_surface_t *src,
                        int width, int height)
{
    cairo_pattern_t *pattern;

    if (!src || !cr) return;
    pattern = pic_viewport_pattern(src, width, height);
    if (!pattern) return;

    cairo_set_source(cr, pattern);
    cairo_paint(cr);
    cairo_pattern_destroy(pattern);
}

/*
 * pic_layer_render_basemap - Paint the Black Marble through the viewport.
 */
void pic_layer_render_basemap(cairo_t *cr, int width, int height,
                              time_t now, void *user_data)
{
    cairo_surface_t *map_surface = (cairo_surface_t *)user_data;

    (void)now;

    if (!map_surface) {
        /* No map loaded — fill with near-black as fallback */
        cairo_set_source_rgb(cr, 0.02, 0.02, 0.05);
        cairo_paint(cr);
        return;
    }

    pic_paint_viewport(cr, map_surface, width, height);
}
