/*
 * layers_lightning.c - Lightning strike overlay layer
 *
 * Renders recent lightning strikes from Blitzortung as small
 * lightning bolt icons on the world map. Each strike fades
 * through a colour sequence over its lifetime:
 *
 *   Start:  Bright white  (fresh strike, maximum visibility)
 *   Middle: Yellow        (ageing)
 *   End:    Deep red      (about to disappear)
 *
 * The fade duration is configurable (default 3 seconds) and
 * can be changed from the dashboard. After the fade completes,
 * the strike is no longer drawn.
 *
 * The lightning bolt shape is drawn procedurally with Cairo
 * path operations — no image assets required. The bolt is
 * small (scaled to display height) to avoid cluttering the
 * map during heavy storm activity.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "lightning.h"
#include <cairo/cairo.h>
#include <math.h>
#include <sys/time.h>

#include "math_utils.h"

/*
 * fade_colour - Interpolate the strike colour based on age fraction.
 *
 * Maps the normalised age (0.0 = just struck, 1.0 = about to expire)
 * through a three-stop gradient:
 *
 *   0.0 → 0.3:  White (1,1,1) to Yellow (1,1,0)
 *   0.3 → 0.7:  Yellow (1,1,0) to Deep Red (0.8,0,0)
 *   0.7 → 1.0:  Deep Red fading to transparent
 *
 * The alpha channel starts at 1.0 and fades linearly to 0.0
 * over the final 30% of the fade window, giving a smooth
 * disappearance rather than a hard pop-off.
 */
static void fade_colour(double age_frac, double *r, double *g,
                        double *b, double *a)
{
    if (age_frac < 0.0) age_frac = 0.0;
    if (age_frac > 1.0) age_frac = 1.0;

    if (age_frac < 0.3) {
        /* White → Yellow: blue channel drops from 1 to 0 */
        double t = age_frac / 0.3;
        *r = 1.0;
        *g = 1.0;
        *b = 1.0 - t;
    } else if (age_frac < 0.7) {
        /* Yellow → Deep Red: green channel drops, red stays high */
        double t = (age_frac - 0.3) / 0.4;
        *r = 1.0 - 0.2 * t;   /* 1.0 → 0.8 */
        *g = 1.0 - t;          /* 1.0 → 0.0 */
        *b = 0.0;
    } else {
        /* Deep Red, fading out */
        *r = 0.8;
        *g = 0.0;
        *b = 0.0;
    }

    /* Alpha: full opacity for first 70%, then linear fade to 0 */
    if (age_frac < 0.7) {
        *a = 1.0;
    } else {
        *a = 1.0 - (age_frac - 0.7) / 0.3;
    }
}

/*
 * draw_lightning_bolt - Draw a small lightning bolt icon.
 *
 * The bolt is a simple 5-point zigzag path, filled with the
 * given colour. Size is specified as the total height of the
 * bolt in pixels. The bolt is centred on (cx, cy).
 *
 * Shape (normalised to height h, origin at centre):
 *
 *        *         <- top point
 *       / \
 *      /   \
 *     *-----*     <- upper zigzag
 *      \   /
 *       \ /
 *        *-----*  <- lower zigzag
 *         \   /
 *          \ /
 *           *     <- bottom point
 */
static void draw_lightning_bolt(cairo_t *cr, double cx, double cy,
                                double h, double r, double g,
                                double b, double a)
{
    double w = h * 0.35;   /* Bolt width proportional to height */
    double half_h = h / 2.0;

    cairo_save(cr);
    cairo_translate(cr, cx, cy);

    /* Draw the bolt as a filled polygon.
     * The shape is a classic zigzag lightning bolt. */
    cairo_move_to(cr,  0.0,         -half_h);       /* Top point       */
    cairo_line_to(cr,  w * 0.5,     -half_h * 0.15);/* Upper right     */
    cairo_line_to(cr, -w * 0.15,    -half_h * 0.05);/* Upper left step */
    cairo_line_to(cr,  w * 0.35,     half_h * 0.4); /* Lower right     */
    cairo_line_to(cr, -w * 0.3,      half_h * 0.05);/* Lower left step */
    cairo_line_to(cr,  0.0,          half_h);        /* Bottom point    */
    cairo_line_to(cr, -w * 0.35,     half_h * 0.15);/* Return left     */
    cairo_line_to(cr,  w * 0.15,     half_h * 0.05);/* Return step     */
    cairo_line_to(cr, -w * 0.5,     -half_h * 0.3); /* Upper return    */
    cairo_line_to(cr,  w * 0.15,    -half_h * 0.1); /* Final step      */
    cairo_close_path(cr);

    /* Fill with the fade colour */
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_fill_preserve(cr);

    /* Thin bright outline for visibility on any background */
    cairo_set_source_rgba(cr, r, g, b, a * 0.6);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);

    cairo_restore(cr);
}

/*
 * pic_layer_render_lightning - Render all active lightning strikes.
 *
 * Iterates the circular buffer, skips strikes that have expired
 * (older than fade_ms), and draws each visible strike as a small
 * lightning bolt with colour determined by its age.
 *
 * The layer update interval is set to 1 second in the layer stack,
 * but since strikes fade over just a few seconds we want to
 * re-render frequently. On multi-core Pi the 1-second interval is
 * fine; on single-core Pi the layer still works but the fade
 * animation appears stepped rather than smooth.
 *
 * user_data points to a pic_lightning_t.
 */
void pic_layer_render_lightning(cairo_t *cr, int width, int height,
                                time_t now, void *user_data)
{
    pic_lightning_t *data = (pic_lightning_t *)user_data;
    struct timeval tv_now;
    int i, fade_ms;
    double bolt_h;

    (void)now; /* We use gettimeofday for sub-second precision */

    if (!data) return;

    gettimeofday(&tv_now, NULL);

    /*
     * Bolt size: scale to display height. At 1080p this gives a
     * bolt about 9 pixels tall — small enough not to clutter but
     * visible enough to spot. At 4K it's about 18 pixels.
     */
    bolt_h = height / 120.0;

    pthread_mutex_lock(&data->mutex);
    fade_ms = data->fade_ms;

    for (i = 0; i < data->count; i++) {
        pic_strike_t *s = &data->strikes[i];
        double age_ms, age_frac;
        double r, g, b, a;
        double sx, sy;

        /* Calculate age in milliseconds */
        age_ms = (tv_now.tv_sec - s->when.tv_sec) * 1000.0 +
                 (tv_now.tv_usec - s->when.tv_usec) / 1000.0;

        /* Skip expired strikes */
        if (age_ms < 0 || age_ms > fade_ms) continue;

        /* Normalised age: 0.0 = just struck, 1.0 = about to expire */
        age_frac = age_ms / fade_ms;

        /* Get the fade colour for this age */
        fade_colour(age_frac, &r, &g, &b, &a);

        /* Convert geographic coordinates to screen position */
        sx = pic_lon_to_x(s->lon, width);
        sy = pic_lat_to_y(s->lat, height);

        /*
         * Attract effect: on the very first frame (age < 1 second),
         * draw a large bright white bolt at 3x size to grab
         * attention. After that, normal size with the colour fade.
         */
        if (age_ms < 1000.0) {
            draw_lightning_bolt(cr, sx, sy, bolt_h * 3.0,
                                1.0, 1.0, 1.0, 1.0);
        } else {
            draw_lightning_bolt(cr, sx, sy, bolt_h, r, g, b, a);
        }
    }

    pthread_mutex_unlock(&data->mutex);
}
