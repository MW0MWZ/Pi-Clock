/*
 * layers_timezone.c - UTC time zone overlay layer
 *
 * Draws real-world time zone boundaries from Natural Earth data
 * as dotted blue lines, and labels each zone with its UTC offset
 * along the top edge of the display.
 *
 * The boundary data is loaded from a binary file (same format as
 * country borders — see borders.h) generated at build time from
 * Natural Earth's 10m time zones dataset. These boundaries follow
 * actual political borders, not neat 15-degree verticals.
 *
 * UTC offset labels are positioned at the theoretical zone centres
 * (every 15 degrees) since that's where the label logically belongs,
 * even though the actual boundary may be offset.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "borders.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <math.h>

/*
 * pic_layer_render_timezone - Render timezone boundaries and labels.
 *
 * The user_data pointer holds an pic_borders_t struct containing
 * the timezone boundary polygons loaded at startup.
 */
void pic_layer_render_timezone(cairo_t *cr, int width, int height,
                               time_t now, void *user_data)
{
    pic_borders_t *tz_data = (pic_borders_t *)user_data;
    uint32_t i;
    int zone;
    double font_size;
    double half_width;
    double dashes[] = {3.0, 5.0};

    (void)now;

    /* Scale font relative to display height */
    font_size = height / 70.0;

    half_width = pic_wrap_threshold_px(width);

    /*
     * Phase 1: Draw the actual timezone boundary lines.
     *
     * These come from Natural Earth data and follow real political
     * borders. Drawn as dotted blue lines — a nice mid-blue that
     * stands out from the grey grid but isn't obnoxious.
     */
    if (tz_data && tz_data->num_polygons > 0) {
        cairo_set_dash(cr, dashes, 2, 0);
        /* Blue is perceptually dimmer than other overlay colours,
         * so it needs higher alpha to read at the same intensity. */
        cairo_set_source_rgba(cr, 0.35, 0.55, 1.0, 0.75);
        cairo_set_line_width(cr, 1.0);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

        for (i = 0; i < tz_data->num_polygons; i++) {
            pic_polygon_t *poly = &tz_data->polygons[i];
            uint32_t j;
            double prev_x;

            if (poly->num_points < 2) {
                continue;
            }

            prev_x = pic_lon_to_x((double)poly->lons[0], width);
            cairo_move_to(cr,
                          prev_x,
                          pic_lat_to_y((double)poly->lats[0], height));

            for (j = 1; j < poly->num_points; j++) {
                double x = pic_lon_to_x((double)poly->lons[j], width);
                double y = pic_lat_to_y((double)poly->lats[j], height);

                /* Break line at the wrap-around seam */
                if (fabs(x - prev_x) > half_width) {
                    cairo_move_to(cr, x, y);
                } else {
                    cairo_line_to(cr, x, y);
                }

                prev_x = x;
            }
        }

        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }

    /*
     * Phase 2: Draw UTC offset labels along the top edge.
     *
     * Labels are positioned at the theoretical zone centres (every
     * 15 degrees) regardless of where the actual boundaries fall.
     * This keeps them evenly spaced and readable.
     */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    for (zone = -12; zone <= 11; zone++) {
        double center_x;
        char label[16];
        cairo_text_extents_t extents;
        double text_x;

        center_x = pic_lon_to_x(zone * 15.0, width);

        /*
         * Skip labels that would render off-screen or too close
         * to the edges where they'd overlap with the wrapped side.
         */
        if (center_x < font_size || center_x > width - font_size) {
            continue;
        }

        if (zone == 0) {
            snprintf(label, sizeof(label), "UTC");
        } else if (zone > 0) {
            snprintf(label, sizeof(label), "+%d", zone);
        } else {
            snprintf(label, sizeof(label), "%d", zone);
        }

        cairo_text_extents(cr, label, &extents);
        text_x = center_x - extents.width / 2.0;

        /* Clamp to screen bounds so labels don't clip at edges */
        if (text_x < 2) text_x = 2;
        if (text_x + extents.width > width - 2) text_x = width - extents.width - 2;

        /* Blue labels matching the boundary line colour. */
        cairo_set_source_rgba(cr, 0.3, 0.5, 1.0, 0.9);
        cairo_move_to(cr, text_x, font_size + 3);
        cairo_show_text(cr, label);
    }
}
