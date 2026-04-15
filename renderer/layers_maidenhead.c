/*
 * layers_maidenhead.c - Maidenhead grid square overlay
 *
 * Draws the Maidenhead Locator System grid over the map. At world
 * scale, shows the 2-character field grid (18x18 fields, each
 * 20 degrees longitude by 10 degrees latitude, labelled AA to RR).
 *
 * The grid is drawn as a subtle overlay with field labels at each
 * intersection. Lines are drawn in a distinct colour (green) to
 * distinguish from the lat/lon grid (grey) and timezone boundaries
 * (blue).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include <cairo/cairo.h>
#include <stdio.h>

/*
 * Maidenhead field grid:
 *   Longitude: 18 fields of 20 degrees each (-180 to +180)
 *              Fields A (180W) through R (160E)
 *   Latitude:  18 fields of 10 degrees each (-90 to +90)
 *              Fields A (90S) through R (80N)
 *
 * The first character is the longitude field (A-R)
 * The second character is the latitude field (A-R)
 * e.g., IO = lon field I (10W-10E), lat field O (50N-60N)
 */

void pic_layer_render_maidenhead(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data)
{
    int lon_field, lat_field;
    double font_size;
    double dashes[] = {2.0, 4.0};

    (void)now;
    (void)user_data;

    font_size = height / 100.0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    /* Draw field boundaries and labels */
    for (lon_field = 0; lon_field < 18; lon_field++) {
        double lon_deg = -180.0 + lon_field * 20.0;
        double x = pic_lon_to_x(lon_deg, width);

        /* Vertical field boundary — dashed green line */
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_set_source_rgba(cr, 0.3, 0.8, 0.3, 0.2);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, height);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);

        for (lat_field = 0; lat_field < 18; lat_field++) {
            double lat_deg = -90.0 + lat_field * 10.0;
            double y = pic_lat_to_y(lat_deg, height);

            /* Horizontal boundary — only draw once per lat_field */
            if (lon_field == 0) {
                cairo_set_dash(cr, dashes, 2, 0);
                cairo_set_source_rgba(cr, 0.3, 0.8, 0.3, 0.2);
                cairo_set_line_width(cr, 0.5);
                cairo_move_to(cr, 0, y);
                cairo_line_to(cr, width, y);
                cairo_stroke(cr);
                cairo_set_dash(cr, NULL, 0, 0);
            }

            /* Draw the 2-character field label at the centre of
             * each grid square. Only draw every other square to
             * avoid excessive clutter at world scale. */
            if ((lon_field + lat_field) % 2 == 0) {
                char label[4];
                double cx, cy;

                label[0] = 'A' + lon_field;
                label[1] = 'A' + lat_field;
                label[2] = '\0';

                /* Centre of the field */
                cx = pic_lon_to_x(lon_deg + 10.0, width);
                cy = pic_lat_to_y(lat_deg + 5.0, height);

                cairo_set_source_rgba(cr, 0.3, 0.8, 0.3, 0.3);
                cairo_move_to(cr, cx - font_size * 0.6, cy + font_size * 0.3);
                cairo_show_text(cr, label);
            }
        }
    }
}
