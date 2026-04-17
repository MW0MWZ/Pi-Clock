/*
 * layers_dxspots.c - DX cluster spot overlay layer
 *
 * Draws active DX spots on the map:
 *   - Great circle arc from spotter to DX station
 *   - Coloured dot at the DX station location
 *   - DX callsign label near the dot
 *   - Colour coded by band
 *   - Opacity fades as spot ages toward expiry
 *
 * The spot list is populated by the DX cluster telnet client
 * running in a background thread (dxcluster.c).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "display.h"
#include "config.h"
#include "dxspot.h"
#include "newsticker.h"
#include <cairo/cairo.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "math_utils.h"

/* Number of waypoints for great circle arcs */
#define ARC_POINTS 40

/* Band legend uses the canonical table in dxspot.c via
 * pic_band_name() and pic_band_colour() — single source of truth. */

/*
 * great_circle_waypoint - Interpolate along a great circle arc.
 *
 * Computes the point at fraction f (0=start, 1=end) along the
 * great circle connecting two lat/lon points. Uses spherical
 * linear interpolation (Slerp) via 3D unit vectors:
 *   A = sin((1-f)*d) / sin(d)   (weight for start point)
 *   B = sin(f*d) / sin(d)       (weight for end point)
 * where d is the angular distance. Result is converted back
 * from Cartesian (x,y,z) to lat/lon.
 */
static void great_circle_waypoint(double lat1, double lon1,
                                  double lat2, double lon2,
                                  double f, double *lat_out, double *lon_out)
{
    double phi1 = lat1 * DEG_TO_RAD;
    double lam1 = lon1 * DEG_TO_RAD;
    double phi2 = lat2 * DEG_TO_RAD;
    double lam2 = lon2 * DEG_TO_RAD;
    double d, A, B, x, y, z;

    /* Angular distance between the two points */
    d = acos(sin(phi1) * sin(phi2) +
             cos(phi1) * cos(phi2) * cos(lam2 - lam1));

    if (d < 1e-6) {
        *lat_out = lat1;
        *lon_out = lon1;
        return;
    }

    A = sin((1.0 - f) * d) / sin(d);
    B = sin(f * d) / sin(d);

    x = A * cos(phi1) * cos(lam1) + B * cos(phi2) * cos(lam2);
    y = A * cos(phi1) * sin(lam1) + B * cos(phi2) * sin(lam2);
    z = A * sin(phi1) + B * sin(phi2);

    *lat_out = atan2(z, sqrt(x * x + y * y)) * RAD_TO_DEG;
    *lon_out = atan2(y, x) * RAD_TO_DEG;
}

/*
 * pic_layer_render_dxspots - Render active DX spots on the map.
 *
 * user_data points to an pic_spotlist_t.
 */
void pic_layer_render_dxspots(cairo_t *cr, int width, int height,
                              time_t now, void *user_data)
{
    pic_spotlist_t *spots = (pic_spotlist_t *)user_data;
    double font_size = height / 120.0;
    double dot_radius = height / 200.0;
    double half_width = pic_wrap_threshold_px(width);
    int i, j;

    if (!spots) return;

    /* Expire old spots */
    pic_spotlist_expire(spots, now);

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    pthread_mutex_lock(&spots->mutex);

    for (i = 0; i < MAX_SPOTS; i++) {
        pic_dxspot_t *spot = &spots->spots[i];
        double r, g, b, a;
        double age_fraction;
        double dx_x, dx_y;

        if (!spot->active) continue;

        /* Calculate opacity based on age (fade over last 5 minutes) */
        {
            int age = (int)(now - spot->timestamp);
            if (age > pic_spot_max_age - 300) {
                /* Last 5 minutes: linear fade from 1.0 to 0.0 */
                age_fraction = 1.0 - (double)(age - (pic_spot_max_age - 300)) / 300.0;
                if (age_fraction < 0.0) age_fraction = 0.0;
            } else {
                age_fraction = 1.0;
            }
        }

        /* Get band colour */
        pic_band_colour(spot->band_index, &r, &g, &b, &a);
        a *= age_fraction;

        /* Draw great circle arc from spotter to DX station */
        cairo_set_source_rgba(cr, r, g, b, a * 0.4);
        cairo_set_line_width(cr, height / 540.0);

        {
            double prev_x = -1;
            int first = 1;

            for (j = 0; j <= ARC_POINTS; j++) {
                double f = (double)j / ARC_POINTS;
                double wlat, wlon, wx, wy;

                great_circle_waypoint(spot->spotter_lat, spot->spotter_lon,
                                      spot->dx_lat, spot->dx_lon,
                                      f, &wlat, &wlon);

                wx = pic_lon_to_x(wlon, width);
                wy = pic_lat_to_y(wlat, height);

                /* Break at antimeridian crossing */
                if (!first && fabs(wx - prev_x) > half_width) {
                    cairo_stroke(cr);
                    cairo_move_to(cr, wx, wy);
                } else if (first) {
                    cairo_move_to(cr, wx, wy);
                    first = 0;
                } else {
                    cairo_line_to(cr, wx, wy);
                }

                prev_x = wx;
            }
            cairo_stroke(cr);
        }

        /* Draw dot at DX station location with stacking offset.
         * Count how many previous active spots share similar coordinates
         * (within 3 degrees) and offset this one vertically. */
        dx_x = pic_lon_to_x(spot->dx_lon, width);
        dx_y = pic_lat_to_y(spot->dx_lat, height);

        {
            int stack_idx = 0, si;
            for (si = 0; si < i; si++) {
                if (spots->spots[si].active &&
                    fabs(spots->spots[si].dx_lat - spot->dx_lat) < 3.0 &&
                    fabs(spots->spots[si].dx_lon - spot->dx_lon) < 3.0) {
                    stack_idx++;
                }
            }
            dx_y += stack_idx * (font_size + 2);
        }

        cairo_arc(cr, dx_x, dx_y, dot_radius, 0, 2 * PI);
        cairo_set_source_rgba(cr, r, g, b, a);
        cairo_fill(cr);

        /* Draw callsign label with dark background for readability */
        {
            cairo_text_extents_t ext;
            double lx = dx_x + dot_radius + 3;
            double ly = dx_y + font_size * 0.3;

            cairo_text_extents(cr, spot->dx_call, &ext);

            /* Background pill */
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, a * 0.75);
            {
                /* Rounded rectangle */
                double rx = lx - 3;
                double ry = ly - ext.height - 2;
                double rw = ext.width + 6;
                double rh = ext.height + 5;
                double cr_r = 3.0;
                cairo_move_to(cr, rx + cr_r, ry);
                cairo_line_to(cr, rx + rw - cr_r, ry);
                cairo_arc(cr, rx + rw - cr_r, ry + cr_r, cr_r, -PI/2, 0);
                cairo_line_to(cr, rx + rw, ry + rh - cr_r);
                cairo_arc(cr, rx + rw - cr_r, ry + rh - cr_r, cr_r, 0, PI/2);
                cairo_line_to(cr, rx + cr_r, ry + rh);
                cairo_arc(cr, rx + cr_r, ry + rh - cr_r, cr_r, PI/2, PI);
                cairo_line_to(cr, rx, ry + cr_r);
                cairo_arc(cr, rx + cr_r, ry + cr_r, cr_r, PI, 3*PI/2);
                cairo_close_path(cr);
            }
            cairo_fill(cr);

            /* Callsign text */
            cairo_set_source_rgba(cr, r, g, b, a * 0.9);
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, spot->dx_call);
        }

        /* Highlight spots from home country with a ring */
        if (spot->is_home_country) {
            cairo_arc(cr, dx_x, dx_y, dot_radius + 2, 0, 2 * PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, a * 0.6);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
    }

    pthread_mutex_unlock(&spots->mutex);
}

/*
 * pic_layer_render_dxspots_legend - Draw the DX cluster band legend.
 *
 * Separated from the main render function so it can be drawn
 * post-composite at full opacity, independent of layer opacity.
 */
void pic_layer_render_dxspots_legend(cairo_t *cr, int width, int height,
                                     time_t now, void *user_data)
{
    pic_spotlist_t *spots = (pic_spotlist_t *)user_data;
    double legend_fs = height / 80.0;
    double dot_r = legend_fs * 0.35;
    double dot_gap = 4;
    double item_gap = legend_fs * 1.2;
    double bg_pad = legend_fs * 0.5;
    int li, enabled_count = 0;

    (void)now;
    if (!spots) return;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, legend_fs * 0.75);

    {
        const char *legend_title = "DX Cluster";
        cairo_text_extents_t title_ext;
        cairo_text_extents(cr, legend_title, &title_ext);

        double total_content_w = title_ext.x_advance + item_gap;
        for (li = 0; li < pic_band_count(); li++) {
            cairo_text_extents_t ext;
            if (!(spots->band_mask & (1u << li))) continue;
            cairo_text_extents(cr, pic_band_name(li), &ext);
            total_content_w += dot_r * 2 + dot_gap + ext.x_advance;
            enabled_count++;
        }

        if (enabled_count > 0) {
            double legend_w = total_content_w + (enabled_count - 1) * item_gap;
            double lx = (width - legend_w) / 2.0;
            double ly, cx;

            {
                double ref_bottom;
                double bg_h = legend_fs * 1.2 + bg_pad * 2;
                double gap = height / 160.0;

                if (pic_ticker_bar_top > 0) {
                    ref_bottom = pic_ticker_bar_top - gap;
                } else {
                    ref_bottom = height - height / 80.0;
                }

                ly = ref_bottom - bg_h + bg_pad + dot_r;
            }
            cx = lx;

            /* Background pill */
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.05, 0.7);
            {
                double bx = lx - bg_pad;
                double by = ly - bg_pad - dot_r;
                double bw = legend_w + bg_pad * 2;
                double bh = legend_fs * 1.2 + bg_pad * 2;
                double cr_r = bg_pad;
                cairo_move_to(cr, bx + cr_r, by);
                cairo_line_to(cr, bx + bw - cr_r, by);
                cairo_arc(cr, bx + bw - cr_r, by + cr_r, cr_r, -PI/2, 0);
                cairo_line_to(cr, bx + bw, by + bh - cr_r);
                cairo_arc(cr, bx + bw - cr_r, by + bh - cr_r, cr_r, 0, PI/2);
                cairo_line_to(cr, bx + cr_r, by + bh);
                cairo_arc(cr, bx + cr_r, by + bh - cr_r, cr_r, PI/2, PI);
                cairo_line_to(cr, bx, by + cr_r);
                cairo_arc(cr, bx + cr_r, by + cr_r, cr_r, PI, 3*PI/2);
                cairo_close_path(cr);
            }
            cairo_fill(cr);

            /* Title */
            cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
            cairo_move_to(cr, cx, ly + legend_fs * 0.25);
            cairo_show_text(cr, legend_title);
            cx += title_ext.x_advance + item_gap;

            /* Band dots + labels */
            for (li = 0; li < pic_band_count(); li++) {
                cairo_text_extents_t ext;
                double br, bg, bb, ba;

                if (!(spots->band_mask & (1u << li))) continue;

                pic_band_colour(li, &br, &bg, &bb, &ba);
                cairo_text_extents(cr, pic_band_name(li), &ext);

                cairo_arc(cr, cx + dot_r, ly, dot_r, 0, 2 * PI);
                cairo_set_source_rgba(cr, br, bg, bb, 0.9);
                cairo_fill(cr);

                cairo_set_source_rgba(cr, br, bg, bb, 0.85);
                cairo_move_to(cr, cx + dot_r * 2 + dot_gap,
                              ly + legend_fs * 0.25);
                cairo_show_text(cr, pic_band_name(li));

                cx += dot_r * 2 + dot_gap + ext.x_advance + item_gap;
            }
        }
    }
}
