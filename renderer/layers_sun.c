/*
 * layers_sun.c - Sub-solar point marker layer
 *
 * Draws a small sun marker at the point on Earth's surface where
 * the sun is directly overhead (the sub-solar point). This moves
 * westward at ~15 degrees per hour and north/south with the seasons.
 *
 * The marker is a filled circle with radiating rays, rendered in
 * a warm yellow/orange colour.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "solar.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>

#include "math_utils.h"

/*
 * pic_layer_render_sun - Draw the sub-solar point marker.
 */
void pic_layer_render_sun(cairo_t *cr, int width, int height,
                          time_t now, void *user_data)
{
    pic_solar_position_t sun;
    double subsolar_lon, subsolar_lat;
    double x, y, r;
    int i;

    (void)user_data;

    sun = pic_solar_position(now);

    /* Convert solar position to geographic coordinates.
     * GHA is measured westward from Greenwich, so sub-solar
     * longitude = -GHA (converted to degrees). */
    subsolar_lon = -sun.gha * RAD_TO_DEG;
    if (subsolar_lon < -180.0) subsolar_lon += 360.0;
    subsolar_lat = sun.declination * RAD_TO_DEG;

    /* Convert to pixel coordinates */
    x = pic_lon_to_x(subsolar_lon, width);
    y = pic_lat_to_y(subsolar_lat, height);

    /* Marker size scales with display height */
    r = height / 160.0;

    /* Draw radiating rays */
    cairo_set_source_rgba(cr, 1.0, 0.85, 0.2, 0.6);
    cairo_set_line_width(cr, 1.5);

    for (i = 0; i < 8; i++) {
        double angle = i * PI / 4.0;
        double inner = r * 1.4;
        double outer = r * 2.2;

        cairo_move_to(cr, x + cos(angle) * inner, y + sin(angle) * inner);
        cairo_line_to(cr, x + cos(angle) * outer, y + sin(angle) * outer);
    }
    cairo_stroke(cr);

    /* Draw filled circle at the sub-solar point */
    cairo_arc(cr, x, y, r, 0, 2 * PI);
    cairo_set_source_rgba(cr, 1.0, 0.9, 0.3, 0.9);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 0.7, 0.0, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}
