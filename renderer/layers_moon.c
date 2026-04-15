/*
 * layers_moon.c - Moon position and phase display layer
 *
 * Draws the sub-lunar point on the map with a phase indicator.
 * The marker shows the current illumination fraction — a filled
 * circle that's partially lit/dark matching the real moon phase.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "lunar.h"
#include "config.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>

#include "math_utils.h"

/*
 * pic_layer_render_moon - Draw the sub-lunar point and phase.
 */
void pic_layer_render_moon(cairo_t *cr, int width, int height,
                           time_t now, void *user_data)
{
    pic_lunar_position_t moon;
    double sublunar_lon, sublunar_lat;
    double x, y, r;

    (void)user_data;

    moon = pic_lunar_position(now);

    /* Convert to geographic coordinates */
    sublunar_lon = -moon.gha * RAD_TO_DEG;
    if (sublunar_lon < -180.0) sublunar_lon += 360.0;
    sublunar_lat = moon.declination * RAD_TO_DEG;

    /* Convert to pixel coordinates */
    x = pic_lon_to_x(sublunar_lon, width);
    y = pic_lat_to_y(sublunar_lat, height);

    /* Marker size */
    r = height / 180.0;

    /*
     * Draw the moon marker with phase indication.
     *
     * Draw a circle representing the moon. The lit portion is
     * shown in light grey, the dark portion in dark grey.
     * Phase 0.0 = new (all dark), 0.5 = full (all lit).
     */

    /* Dark background circle (the unlit portion) */
    cairo_arc(cr, x, y, r, 0, 2 * PI);
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 0.9);
    cairo_fill(cr);

    /* Lit portion — draw a partial fill based on phase.
     * We approximate the terminator as a vertical ellipse
     * that shifts from left to right as the phase progresses. */
    {
        /*
         * Phase to terminator position:
         *   phase 0.0 (new):  no lit area
         *   phase 0.25 (1st quarter): right half lit
         *   phase 0.5 (full):  all lit
         *   phase 0.75 (3rd quarter): left half lit
         *
         * We use the illumination fraction directly:
         * draw a clipping arc that covers the lit portion.
         */
        double illum = moon.phase; /* 0 to 1 */

        if (illum > 0.02) {
            /* Draw the lit portion as a lighter circle with clipping */
            cairo_save(cr);

            /* Create a clipping region for the lit half.
             * For simplicity, we draw a full circle for >50% illumination
             * and a partial circle for <50%. */
            if (illum >= 0.5) {
                /* More than half lit — full circle, just vary brightness */
                cairo_arc(cr, x, y, r, 0, 2 * PI);
                cairo_set_source_rgba(cr, 0.85, 0.85, 0.8,
                                     0.5 + illum * 0.4);
                cairo_fill(cr);
            } else {
                /* Less than half — draw right-side crescent */
                double w = illum * 2.0 * r; /* width of lit area */
                cairo_arc(cr, x, y, r, -PI / 2, PI / 2);
                cairo_line_to(cr, x + w - r, y + r);
                cairo_line_to(cr, x + w - r, y - r);
                cairo_close_path(cr);
                cairo_set_source_rgba(cr, 0.85, 0.85, 0.8, 0.7);
                cairo_fill(cr);
            }

            cairo_restore(cr);
        }
    }

    /* Outline */
    cairo_arc(cr, x, y, r, 0, 2 * PI);
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.7);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Phase label below the marker */
    {
        char label[16];
        int pct = (int)(moon.phase * 100 + 0.5);
        double font_size = height / 150.0;

        if (pct < 5) {
            snprintf(label, sizeof(label), "New");
        } else if (pct > 95) {
            snprintf(label, sizeof(label), "Full");
        } else {
            snprintf(label, sizeof(label), "%d%%", pct);
        }

        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);
        cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.7);

        {
            cairo_text_extents_t ext;
            cairo_text_extents(cr, label, &ext);
            cairo_move_to(cr, x - ext.width / 2, y + r + font_size + 2);
            cairo_show_text(cr, label);
        }
    }
}
