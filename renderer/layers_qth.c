/*
 * layers_qth.c - Home QTH marker layer
 *
 * Draws the user's home location on the map with a distinctive
 * marker: a filled circle with a crosshair and the callsign label.
 *
 * Reads QTH_LAT, QTH_LON, and CALLSIGN from the renderer config.
 * Re-reads on config reload so it moves when the user changes QTH.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "math_utils.h"

/* Cached QTH position and callsign */
static double qth_lat = 0, qth_lon = 0;
static char qth_call[16] = "";
static time_t qth_last_read = 0;

/* Read QTH and callsign from config. Cached for 60 seconds; cleared
 * by pic_qth_invalidate() on config reload for immediate update. */
static void read_qth_config(time_t now)
{
    FILE *cf;
    char line[256];

    if (now - qth_last_read < 60) return;
    qth_last_read = now;

    cf = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (!cf) return;

    while (fgets(line, sizeof(line), cf)) {
        if (strncmp(line, "QTH_LAT=", 8) == 0)
            qth_lat = atof(line + 8);
        if (strncmp(line, "QTH_LON=", 8) == 0)
            qth_lon = atof(line + 8);
        if (strncmp(line, "CALLSIGN=", 9) == 0) {
            sscanf(line + 9, "%15s", qth_call);
        }
    }
    fclose(cf);
}

/* Called by display.c on config reload */
void pic_qth_invalidate(void)
{
    qth_last_read = 0;
}

/*
 * pic_layer_render_qth - Render the home QTH marker.
 */
void pic_layer_render_qth(cairo_t *cr, int width, int height,
                          time_t now, void *user_data)
{
    double x, y;
    double marker_r = height / 180.0;
    double cross_r = marker_r * 1.8;
    double font_size = height / 130.0;

    (void)user_data;

    read_qth_config(now);

    /* Skip if QTH is unset. Note: also skips the unlikely case of a
     * user at exactly 0°N 0°E (Gulf of Guinea) — acceptable trade-off. */
    if (qth_lat == 0 && qth_lon == 0) return;

    x = pic_lon_to_x(qth_lon, width);
    y = pic_lat_to_y(qth_lat, height);

    /* Outer ring — white with slight transparency */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
    cairo_set_line_width(cr, height / 400.0);
    cairo_arc(cr, x, y, marker_r, 0, 2 * PI);
    cairo_stroke(cr);

    /* Inner filled circle — accent blue */
    cairo_set_source_rgba(cr, 0.3, 0.6, 1.0, 0.8);
    cairo_arc(cr, x, y, marker_r * 0.5, 0, 2 * PI);
    cairo_fill(cr);

    /* Crosshair lines */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
    cairo_set_line_width(cr, height / 800.0);

    cairo_move_to(cr, x - cross_r, y);
    cairo_line_to(cr, x - marker_r * 1.3, y);
    cairo_stroke(cr);

    cairo_move_to(cr, x + marker_r * 1.3, y);
    cairo_line_to(cr, x + cross_r, y);
    cairo_stroke(cr);

    cairo_move_to(cr, x, y - cross_r);
    cairo_line_to(cr, x, y - marker_r * 1.3);
    cairo_stroke(cr);

    cairo_move_to(cr, x, y + marker_r * 1.3);
    cairo_line_to(cr, x, y + cross_r);
    cairo_stroke(cr);

    /* Callsign label */
    if (qth_call[0]) {
        cairo_text_extents_t ext;
        double lx, ly;

        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, font_size);
        cairo_text_extents(cr, qth_call, &ext);

        lx = x - ext.width / 2.0;
        ly = y + cross_r + font_size + 2;

        /* Dark background pill */
        {
            double px = lx - 4;
            double py = ly - ext.height - 2;
            double pw = ext.width + 8;
            double ph = ext.height + 6;
            double pr = 3.0;

            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.75);
            cairo_move_to(cr, px + pr, py);
            cairo_line_to(cr, px + pw - pr, py);
            cairo_arc(cr, px + pw - pr, py + pr, pr, -PI/2, 0);
            cairo_line_to(cr, px + pw, py + ph - pr);
            cairo_arc(cr, px + pw - pr, py + ph - pr, pr, 0, PI/2);
            cairo_line_to(cr, px + pr, py + ph);
            cairo_arc(cr, px + pr, py + ph - pr, pr, PI/2, PI);
            cairo_line_to(cr, px, py + pr);
            cairo_arc(cr, px + pr, py + pr, pr, PI, 3*PI/2);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        /* Callsign text */
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, qth_call);
    }
}
