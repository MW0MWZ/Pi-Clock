/*
 * layers_satellite.c - Satellite tracking overlay layer
 *
 * Draws tracked satellites on the map:
 *   - Ground track line (25% past orbit fading, 75% future orbit)
 *   - Filled dot at the current sub-satellite point
 *   - Dashed circle showing the visibility footprint
 *   - Satellite name + altitude label
 *   - Different colours per satellite
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "satellite.h"
#include <cairo/cairo.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "math_utils.h"

/* Colour palette for satellites */
static const struct { double r, g, b; } sat_colours[] = {
    {1.0, 0.4, 0.1},   /* Orange (ISS) */
    {0.2, 0.8, 1.0},   /* Cyan */
    {1.0, 1.0, 0.3},   /* Yellow */
    {0.5, 1.0, 0.5},   /* Light green */
    {1.0, 0.5, 1.0},   /* Pink */
    {0.4, 0.6, 1.0},   /* Light blue */
    {1.0, 0.8, 0.4},   /* Gold */
    {0.7, 0.4, 1.0},   /* Purple */
};
#define NUM_SAT_COLOURS 8

/*
 * pic_layer_render_satellite - Draw all tracked satellites on the map.
 *
 * For each satellite with a loaded TLE:
 *   1. Propagate orbit to current time (SGP4).
 *   2. Draw ground track: 25% past (faded) + 75% future (brighter).
 *   3. Draw dashed footprint circle (RF coverage at 0° elevation).
 *   4. Draw satellite icon at the sub-satellite point.
 *   5. Draw name + altitude label with dark background pill.
 *
 * Satellites are colour-coded by index in the registered list.
 *   user_data - Pointer to pic_satlist_t.
 */
void pic_layer_render_satellite(cairo_t *cr, int width, int height,
                                time_t now, void *user_data)
{
    pic_satlist_t *sats = (pic_satlist_t *)user_data;
    double dot_r = height / 150.0;
    double font_size = height / 120.0;
    double half_w = pic_wrap_threshold_px(width);
    int i, j;

    if (!sats) return;

    /* Propagate all satellites to current time */
    pic_sat_propagate(sats, now);

    pthread_mutex_lock(&sats->mutex);

    for (i = 0; i < sats->count; i++) {
        pic_sat_t *sat = &sats->sats[i];
        double x, y;
        double cr_r, cr_g, cr_b;
        int ci;

        if (!sat->enabled || !sat->tle_loaded) continue;

        x = pic_lon_to_x(sat->lon, width);
        y = pic_lat_to_y(sat->lat, height);

        ci = i % NUM_SAT_COLOURS;
        cr_r = sat_colours[ci].r;
        cr_g = sat_colours[ci].g;
        cr_b = sat_colours[ci].b;

        /* Ground track — past portion fades, future is brighter */
        if (sat->track_count > 1) {
            cairo_set_line_width(cr, height / 500.0);

            for (j = 1; j < sat->track_count; j++) {
                double px0 = pic_lon_to_x(sat->track_lon[j-1], width);
                double py0 = pic_lat_to_y(sat->track_lat[j-1], height);
                double px1 = pic_lon_to_x(sat->track_lon[j], width);
                double py1 = pic_lat_to_y(sat->track_lat[j], height);
                double alpha;

                /* Skip antimeridian crossings */
                if (fabs(px1 - px0) > half_w) continue;

                /* Past track fades, future track is brighter */
                if (j < sat->track_now_idx) {
                    /* Past: fade from 0.1 to 0.3 */
                    alpha = 0.1 + 0.2 * (double)j / sat->track_now_idx;
                } else {
                    /* Future: 0.4, fading toward the end */
                    double frac = (double)(j - sat->track_now_idx) /
                                  (sat->track_count - sat->track_now_idx);
                    alpha = 0.4 * (1.0 - frac * 0.7);
                }

                cairo_set_source_rgba(cr, cr_r, cr_g, cr_b, alpha);
                cairo_move_to(cr, px0, py0);
                cairo_line_to(cr, px1, py1);
                cairo_stroke(cr);
            }
        }

        /* Footprint circle */
        if (sat->footprint_r > 0) {
            double dashes[] = {6.0, 4.0};
            int npts = 72;
            int first = 1;
            double prev_px = 0;

            cairo_set_source_rgba(cr, cr_r, cr_g, cr_b, 0.2);
            cairo_set_line_width(cr, height / 600.0);
            cairo_set_dash(cr, dashes, 2, 0);

            /* Step around the footprint circle in degrees. On a sphere,
             * 1° longitude = cos(lat) × 1° latitude in physical distance.
             * We divide the lon offset by cos(lat) to compensate so the
             * circle appears round on the equirectangular projection. */
            for (j = 0; j <= npts; j++) {
                double angle = (double)j / npts * 2.0 * PI;
                double flat = sat->lat + sat->footprint_r * sin(angle);
                double flon = sat->lon + sat->footprint_r * cos(angle)
                              / cos(sat->lat * DEG_TO_RAD);
                double px = pic_lon_to_x(flon, width);
                double py = pic_lat_to_y(flat, height);

                if (!first && fabs(px - prev_px) > half_w) {
                    cairo_stroke(cr);
                    cairo_move_to(cr, px, py);
                } else if (first) {
                    cairo_move_to(cr, px, py);
                    first = 0;
                } else {
                    cairo_line_to(cr, px, py);
                }
                prev_px = px;
            }
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
        }

        /* Satellite icon — body + two solar panel wings */
        {
            double s = dot_r * 1.2;  /* Scale unit */
            double angle = 0.4;      /* Slight tilt (radians) */

            cairo_save(cr);
            cairo_translate(cr, x, y);
            cairo_rotate(cr, angle);

            /* Solar panel left */
            cairo_rectangle(cr, -s * 3.0, -s * 0.6, s * 1.8, s * 1.2);
            cairo_set_source_rgba(cr, 0.2, 0.3, 0.7, 0.85);
            cairo_fill(cr);

            /* Solar panel right */
            cairo_rectangle(cr, s * 1.2, -s * 0.6, s * 1.8, s * 1.2);
            cairo_set_source_rgba(cr, 0.2, 0.3, 0.7, 0.85);
            cairo_fill(cr);

            /* Panel grid lines */
            cairo_set_source_rgba(cr, 0.4, 0.5, 0.9, 0.5);
            cairo_set_line_width(cr, s * 0.08);
            cairo_move_to(cr, -s * 2.1, -s * 0.6);
            cairo_line_to(cr, -s * 2.1, s * 0.6);
            cairo_move_to(cr, s * 2.1, -s * 0.6);
            cairo_line_to(cr, s * 2.1, s * 0.6);
            cairo_stroke(cr);

            /* Body (centre rectangle) */
            cairo_rectangle(cr, -s * 1.0, -s * 0.7, s * 2.0, s * 1.4);
            cairo_set_source_rgba(cr, cr_r, cr_g, cr_b, 0.9);
            cairo_fill(cr);

            /* Body outline */
            cairo_rectangle(cr, -s * 1.0, -s * 0.7, s * 2.0, s * 1.4);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
            cairo_set_line_width(cr, s * 0.12);
            cairo_stroke(cr);

            /* Antenna nub */
            cairo_move_to(cr, 0, -s * 0.7);
            cairo_line_to(cr, 0, -s * 1.4);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
            cairo_set_line_width(cr, s * 0.15);
            cairo_stroke(cr);

            cairo_restore(cr);
        }

        /* Name + altitude label */
        {
            cairo_text_extents_t ext;
            char label[48];
            double lx, ly;

            cairo_select_font_face(cr, "sans-serif",
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, font_size);

            snprintf(label, sizeof(label), "%s  %.0f km",
                     sat->name, sat->alt_km);
            cairo_text_extents(cr, label, &ext);

            lx = x + dot_r + 4;
            ly = y + font_size * 0.35;

            /* Background pill */
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
            {
                double rx = lx - 3;
                double ry = ly - ext.height - 2;
                double rw = ext.width + 6;
                double rh = ext.height + 5;
                double rr = 3.0;
                cairo_move_to(cr, rx + rr, ry);
                cairo_line_to(cr, rx + rw - rr, ry);
                cairo_arc(cr, rx + rw - rr, ry + rr, rr, -PI/2, 0);
                cairo_line_to(cr, rx + rw, ry + rh - rr);
                cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, PI/2);
                cairo_line_to(cr, rx + rr, ry + rh);
                cairo_arc(cr, rx + rr, ry + rh - rr, rr, PI/2, PI);
                cairo_line_to(cr, rx, ry + rr);
                cairo_arc(cr, rx + rr, ry + rr, rr, PI, 3*PI/2);
                cairo_close_path(cr);
            }
            cairo_fill(cr);

            cairo_set_source_rgba(cr, cr_r, cr_g, cr_b, 0.95);
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, label);
        }
    }

    pthread_mutex_unlock(&sats->mutex);
}
