/*
 * layers_borders.c - Country border overlay layer
 *
 * Draws country boundaries as thin lines over the map. The border
 * data is loaded from a binary file (see borders.h) and passed to
 * this layer via user_data.
 *
 * Each polygon is drawn as a connected series of line segments.
 * Lines are semi-transparent so the underlying map imagery remains
 * visible. All coordinates are projected through pic_lon_to_x()
 * and pic_lat_to_y() to handle center_lon wrapping.
 *
 * Handling the wrap-around:
 *
 *   When a polygon crosses the seam (the longitude opposite
 *   center_lon), consecutive points will have a large jump in
 *   X position. We detect this by checking if the X distance
 *   between consecutive points exceeds half the screen width.
 *   When this happens, we break the line (cairo_move_to instead
 *   of cairo_line_to) to avoid drawing a line that streaks
 *   across the entire screen.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "borders.h"
#include <cairo/cairo.h>
#include <math.h>

/*
 * pic_layer_render_borders - Render country border lines.
 *
 * The user_data pointer holds an pic_borders_t struct containing
 * all the border polygons loaded at startup.
 */
void pic_layer_render_borders(cairo_t *cr, int width, int height,
                              time_t now, void *user_data)
{
    pic_borders_t *borders = (pic_borders_t *)user_data;
    uint32_t i;
    double wrap_px;

    (void)now; /* Borders don't change with time */

    if (!borders || borders->num_polygons == 0) {
        return;
    }

    /* Pixel distance above which two consecutive points are on
     * opposite sides of the longitudinal seam — scales with zoom. */
    wrap_px = pic_wrap_threshold_px(width);

    /* Near-white, high-alpha line — borders are the visual key to
     * the map and need to dominate the basemap, especially over
     * bright land and sunlit ocean. */
    cairo_set_source_rgba(cr, 1.0, 1.0, 0.85, 0.9);
    cairo_set_line_width(cr, 0.8);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    /*
     * Draw each polygon as a polyline.
     *
     * We batch all polygons into a single path and stroke once
     * at the end, which is more efficient than stroking each
     * polygon individually (fewer GPU state changes when running
     * on the real display with OpenGL).
     */
    for (i = 0; i < borders->num_polygons; i++) {
        pic_polygon_t *poly = &borders->polygons[i];
        uint32_t j;
        double prev_x, prev_y;

        if (poly->num_points < 2) {
            continue;
        }

        /* First point: move to start position */
        prev_x = pic_lon_to_x((double)poly->lons[0], width);
        prev_y = pic_lat_to_y((double)poly->lats[0], height);
        cairo_move_to(cr, prev_x, prev_y);

        /* Subsequent points: line to, with wrap detection */
        for (j = 1; j < poly->num_points; j++) {
            double x = pic_lon_to_x((double)poly->lons[j], width);
            double y = pic_lat_to_y((double)poly->lats[j], height);

            /*
             * Detect wrap-around: if consecutive points are more
             * than half a screen width apart horizontally, the line
             * would cross the seam. Break the line instead.
             */
            if (fabs(x - prev_x) > wrap_px) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }

            prev_x = x;
            prev_y = y;
        }
    }

    /* Stroke all border lines at once */
    cairo_stroke(cr);
}
