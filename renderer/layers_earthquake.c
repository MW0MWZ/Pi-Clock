/*
 * layers_earthquake.c - Earthquake overlay layer
 *
 * Renders recent earthquakes from USGS as animated ripple markers
 * on the world map. Each earthquake is shown with:
 *
 *   - A small earthquake icon (seismograph-style zigzag) at the
 *     epicentre, magnitude-scaled
 *   - Concentric ripple circles emanating outward from the epicentre,
 *     like waves on water
 *   - A magnitude label (e.g., "M5.2") next to the icon for M5+
 *
 * The ripple animation cycles continuously while the event is
 * displayed. Events fade out after the configurable display time
 * (default 30 minutes). Opacity decreases linearly over the final
 * 25% of the display window.
 *
 * Ripple speed and count scale with magnitude:
 *   - M4.5-5.0: 2 ripples, small radius
 *   - M5.0-6.0: 3 ripples, medium radius
 *   - M6.0-7.0: 4 ripples, large radius
 *   - M7.0+:    5 ripples, very large radius
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "earthquake.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "math_utils.h"

/*
 * Ripple pulse period in seconds — concentric rings pulse in
 * opacity over this period. At 1fps animation, smooth motion
 * doesn't work, so we use fixed rings with a breathing effect.
 */
#define RIPPLE_PERIOD 4.0

/*
 * Earthquake icon colour: warm orange, evoking seismic warning.
 */
#define QUAKE_R 0.95
#define QUAKE_G 0.55
#define QUAKE_B 0.15

/*
 * draw_earthquake_icon - Draw a seismograph-style zigzag icon.
 *
 * The icon is a horizontal line with a sharp zigzag in the middle,
 * resembling a seismograph trace. Size scales with magnitude.
 *
 *     ────/\──────
 *        /  \/\
 *              \/──
 *
 * Parameters:
 *   cr   - Cairo context
 *   cx   - Centre X
 *   cy   - Centre Y
 *   size - Total width of the icon in pixels
 *   a    - Alpha (opacity)
 */
static void draw_earthquake_icon(cairo_t *cr, double cx, double cy,
                                 double size, double a)
{
    double half = size / 2.0;
    double amp = size * 0.35;   /* Zigzag amplitude */
    double lw = size * 0.08;    /* Line width       */

    if (lw < 1.0) lw = 1.0;

    cairo_new_path(cr);
    cairo_set_source_rgba(cr, QUAKE_R, QUAKE_G, QUAKE_B, a);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    /* Flat approach from left */
    cairo_move_to(cr, cx - half, cy);
    cairo_line_to(cr, cx - half * 0.4, cy);

    /* Sharp zigzag (seismograph trace) */
    cairo_line_to(cr, cx - half * 0.25, cy - amp * 0.6);
    cairo_line_to(cr, cx - half * 0.05, cy + amp);
    cairo_line_to(cr, cx + half * 0.15, cy - amp);
    cairo_line_to(cr, cx + half * 0.3,  cy + amp * 0.5);
    cairo_line_to(cr, cx + half * 0.4,  cy);

    /* Flat exit to right */
    cairo_line_to(cr, cx + half, cy);

    cairo_stroke(cr);

    /* Small filled circle at epicentre for visibility */
    cairo_arc(cr, cx, cy, lw * 1.5, 0, 2 * PI);
    cairo_set_source_rgba(cr, QUAKE_R, QUAKE_G, QUAKE_B, a * 0.8);
    cairo_fill(cr);
}

/*
 * draw_ripples - Draw static concentric rings with a pulse effect.
 *
 * Draws evenly-spaced rings around the epicentre. At 1fps the
 * display can't show smooth expanding animation, so instead we
 * draw fixed rings with opacity that pulses gently over time
 * (breathing effect). Inner rings are brighter, outer rings fade.
 *
 * This looks clean at any frame rate and clearly communicates
 * "something happened here" without jerky motion.
 */
static void draw_ripples(cairo_t *cr, double cx, double cy,
                         double max_radius, int num_ripples,
                         double now_frac, double base_alpha)
{
    int i;
    double pulse;

    /* Gentle pulse: oscillate opacity between 0.6x and 1.0x */
    pulse = 0.8 + 0.2 * sin(now_frac * 2 * PI);

    cairo_set_line_width(cr, 2.0);

    for (i = 0; i < num_ripples; i++) {
        /* Evenly space rings from centre to max_radius */
        double frac = (double)(i + 1) / num_ripples;
        double radius = frac * max_radius;
        double alpha;

        /* Bright at centre, fading toward the edge.
         * Linear falloff with high base brightness so the
         * inner rings really pop against the map. */
        alpha = (1.0 - frac * 0.8) * base_alpha * pulse;
        if (alpha < 0.01) continue;

        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, radius, 0, 2 * PI);
        cairo_set_source_rgba(cr, QUAKE_R, QUAKE_G, QUAKE_B, alpha);
        cairo_stroke(cr);
    }
}

/*
 * pic_layer_render_earthquake - Render all active earthquake events.
 *
 * For each event within the display window:
 *   1. Draw animated ripple circles emanating from the epicentre
 *   2. Draw the seismograph icon at the epicentre
 *   3. Draw the magnitude label for M5.0+
 *
 * Events fade out over the final 25% of the display window.
 * The ripple animation uses fractional seconds from time() plus
 * a high-resolution offset for smooth sub-second motion.
 *
 * user_data points to a pic_earthquake_t.
 */
void pic_layer_render_earthquake(cairo_t *cr, int width, int height,
                                 time_t now, void *user_data)
{
    pic_earthquake_t *data = (pic_earthquake_t *)user_data;
    int i, display_time;
    double now_frac;
    struct timeval tv;
    double font_size;

    if (!data) return;

    /*
     * Get sub-second time for smooth ripple animation.
     * time_t only gives us whole seconds; gettimeofday gives
     * microsecond resolution for the animation phase.
     */
    gettimeofday(&tv, NULL);
    now_frac = fmod((tv.tv_sec + tv.tv_usec / 1000000.0) / RIPPLE_PERIOD, 1.0);

    font_size = height / 120.0;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, font_size);

    pthread_mutex_lock(&data->mutex);
    display_time = data->display_time_s;

    for (i = 0; i < data->count; i++) {
        pic_quake_t *q = &data->quakes[i];
        double age_s, age_frac, alpha;
        double ex, ey;
        double icon_size, max_ripple_r;
        int num_ripples;

        if (!q->active) continue;

        /* Calculate age in seconds */
        age_s = difftime(now, q->origin_time);
        if (age_s < 0) age_s = 0;

        /* Skip events older than the display window */
        if (age_s > display_time) continue;

        /* Fade over the final 25% of the display window */
        age_frac = age_s / display_time;
        if (age_frac < 0.75) {
            alpha = 1.0;
        } else {
            alpha = 1.0 - (age_frac - 0.75) / 0.25;
        }
        if (alpha < 0.01) continue;

        /* Convert coordinates to screen position */
        ex = pic_lon_to_x(q->lon, width);
        ey = pic_lat_to_y(q->lat, height);

        /*
         * Scale icon and ripple size by magnitude:
         *   M4.5: small icon, 2 tight ripples
         *   M6.0: medium icon, 4 wider ripples
         *   M8.0: large icon, 5 very wide ripples
         *
         * The size scaling uses a log-like curve so that the
         * visual difference between M5 and M6 is proportional
         * to the actual energy difference (roughly 30x per unit).
         */
        {
            double mag_scale = (q->magnitude - 4.0) / 4.0;
            if (mag_scale < 0.1) mag_scale = 0.1;
            if (mag_scale > 1.0) mag_scale = 1.0;

            icon_size = height / 80.0 + height / 40.0 * mag_scale;
            max_ripple_r = height / 60.0 + height / 30.0 * mag_scale;
        }

        /* Pack rings tightly — spacing ~4px apart at 1080p.
         * More rings = clearer radar-pulse look. */
        num_ripples = (int)(max_ripple_r / 4.0);
        if (num_ripples < 3) num_ripples = 3;
        if (num_ripples > 30) num_ripples = 30;

        /* Draw ripple circles first (behind the icon) */
        draw_ripples(cr, ex, ey, max_ripple_r, num_ripples,
                     now_frac, alpha);

        /* Draw the seismograph icon at the epicentre */
        draw_earthquake_icon(cr, ex, ey, icon_size, alpha);

        /* Draw magnitude label for M5.0+ */
        if (q->magnitude >= 5.0) {
            char label[16];
            cairo_text_extents_t ext;
            double lx, ly;

            snprintf(label, sizeof(label), "M%.1f", q->magnitude);
            cairo_text_extents(cr, label, &ext);

            lx = ex + icon_size / 2.0 + 4;
            ly = ey + font_size * 0.35;

            /* Dark background pill for readability */
            {
                double rx = lx - 3;
                double ry = ly - ext.height - 2;
                double rw = ext.width + 6;
                double rh = ext.height + 5;
                double cr_r = 3.0;

                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, alpha * 0.7);
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
                cairo_fill(cr);
            }

            /* Magnitude text */
            cairo_set_source_rgba(cr, QUAKE_R, QUAKE_G, QUAKE_B,
                                  alpha * 0.9);
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, label);
        }
    }

    pthread_mutex_unlock(&data->mutex);
}
