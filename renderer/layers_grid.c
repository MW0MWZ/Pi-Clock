/*
 * layers_grid.c - Latitude/longitude grid overlay layer
 *
 * Draws a graticule (grid of latitude and longitude lines) over
 * the map at regular intervals. Lines are drawn as thin,
 * semi-transparent strokes so the map imagery remains visible
 * underneath.
 *
 * Grid lines are drawn every 30 degrees by default:
 *   - Longitude: -180, -150, -120, ..., 0, ..., 120, 150, 180
 *   - Latitude:  -90, -60, -30, 0, 30, 60, 90
 *
 * Special lines are drawn slightly brighter:
 *   - Equator (0 latitude)
 *   - Prime Meridian (0 longitude)
 *   - Tropics of Cancer and Capricorn (23.44 N/S)
 *   - Arctic and Antarctic circles (66.56 N/S)
 *
 * All lines respect center_lon: longitude lines are positioned
 * using pic_lon_to_x() which applies the centering offset.
 *
 * Labels show the degree value at each grid line, positioned
 * near the edges of the display.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <math.h>

/* Grid spacing in degrees — 10 gives 36 longitude and 18 latitude lines */
#define GRID_INTERVAL 10

/*
 * draw_hline - Draw a horizontal line across the full width.
 *
 * Used for latitude lines. The y position is computed from the
 * latitude using pic_lat_to_y().
 */
static void draw_hline(cairo_t *cr, double y, int width)
{
    cairo_move_to(cr, 0, y);
    cairo_line_to(cr, width, y);
}

/*
 * draw_vline - Draw a vertical line from top to bottom.
 *
 * Used for longitude lines. The x position is computed from the
 * longitude using pic_lon_to_x().
 */
static void draw_vline(cairo_t *cr, double x, int height)
{
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, height);
}

/*
 * pic_layer_render_grid - Render the lat/lon grid overlay.
 *
 * Draws grid lines and labels. The grid is a static overlay that
 * doesn't change with time, but we re-render it if center_lon
 * changes (in the future, when live re-centering is supported).
 */
void pic_layer_render_grid(cairo_t *cr, int width, int height,
                           time_t now, void *user_data)
{
    int lon, lat;
    double x, y;
    char label[16];
    double font_size;

    (void)now;
    (void)user_data;

    /* Scale font and line width relative to display height */
    font_size = height / 80.0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    /*
     * Draw longitude lines (vertical).
     *
     * We iterate through all longitudes at GRID_INTERVAL spacing.
     * Each line is converted to an X position using pic_lon_to_x()
     * which handles the center_lon offset automatically.
     */
    for (lon = -180; lon <= 180; lon += GRID_INTERVAL) {
        x = pic_lon_to_x((double)lon, width);

        /* All grid lines use the same style — major/minor only.
         * Alphas tuned for legibility at 4K against bright land. */
        if (lon % 30 == 0) {
            cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 0.5);
            cairo_set_line_width(cr, 0.8);
        } else {
            cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 0.4);
            cairo_set_line_width(cr, 0.5);
        }

        draw_vline(cr, x, height);
        cairo_stroke(cr);

        /* Label every 30 degrees to avoid clutter */
        if (lon > -180 && lon < 180 && (lon % 30 == 0)) {
            if (lon == 0) {
                snprintf(label, sizeof(label), "0");
            } else if (lon > 0) {
                snprintf(label, sizeof(label), "%dE", lon);
            } else {
                snprintf(label, sizeof(label), "%dW", -lon);
            }

            cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.7);
            cairo_move_to(cr, x + 3, height - font_size * 0.5);
            cairo_show_text(cr, label);
        }
    }

    /*
     * Draw latitude lines (horizontal).
     *
     * Latitude is not affected by center_lon — only the horizontal
     * axis shifts. We use pic_lat_to_y() for the conversion.
     */
    for (lat = -90; lat <= 90; lat += GRID_INTERVAL) {
        y = pic_lat_to_y((double)lat, height);

        if (lat % 30 == 0) {
            cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 0.5);
            cairo_set_line_width(cr, 0.8);
        } else {
            cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, 0.4);
            cairo_set_line_width(cr, 0.5);
        }

        draw_hline(cr, y, width);
        cairo_stroke(cr);

        /* Label every 30 degrees to avoid clutter */
        if (lat > -90 && lat < 90 && (lat % 30 == 0 || lat == 0)) {
            if (lat == 0) {
                snprintf(label, sizeof(label), "EQ");
            } else if (lat > 0) {
                snprintf(label, sizeof(label), "%dN", lat);
            } else {
                snprintf(label, sizeof(label), "%dS", -lat);
            }

            cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.7);
            cairo_move_to(cr, 5, y - font_size * 0.3);
            cairo_show_text(cr, label);
        }
    }

    /*
     * Draw special latitude lines: Tropics and Polar circles.
     * These are drawn as dashed lines to distinguish them from
     * the regular grid.
     */
    {
        double dashes[] = {6.0, 4.0};
        double special_lats[] = {23.44, -23.44, 66.56, -66.56};
        int i;

        cairo_set_dash(cr, dashes, 2, 0);
        cairo_set_source_rgba(cr, 0.8, 0.6, 0.3, 0.5);
        cairo_set_line_width(cr, 0.8);

        for (i = 0; i < 4; i++) {
            y = pic_lat_to_y(special_lats[i], height);
            draw_hline(cr, y, width);
            cairo_stroke(cr);
        }

        /* Reset dashes for future drawing */
        cairo_set_dash(cr, NULL, 0, 0);
    }
}
