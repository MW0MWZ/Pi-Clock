/*
 * layers_wind.c - Wind streamline overlay layer
 *
 * Renders global surface wind as flowing streamline traces on the
 * world map. Inspired by earth.nullschool.net's particle animation,
 * but rendered as static streamlines suitable for 1fps display.
 *
 * Visual style:
 *   - Each streamline is a thin curved trace following the wind field
 *   - Bright white at the leading end (wind direction)
 *   - Fading to transparent at the trailing end
 *   - More streamlines in areas of stronger wind (density = speed)
 *   - Stronger winds also get brighter/longer traces
 *
 * The streamlines are computed once when new wind data arrives and
 * cached on a Cairo surface. On subsequent frames the cached surface
 * is simply painted — zero per-frame cost. Wind data updates every
 * 6 hours (from NOAA GFS via GitHub Action), so recomputation is
 * very infrequent.
 *
 * Algorithm:
 *   1. Seed random points across the globe, density weighted by
 *      local wind speed (more seeds where wind is stronger)
 *   2. From each seed, trace 10-15 steps following the U/V field
 *   3. Draw each trace as a thin line with alpha gradient
 *      (bright at head, transparent at tail)
 *
 * Data source: NOAA GFS (public domain), pre-processed by GitHub
 * Action into a compact binary format.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "config.h"
#include "wind.h"
#include <cairo/cairo.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "math_utils.h"

/*
 * Streamline parameters — tuned for visual density at 1080p.
 *
 * MAX_STREAMLINES: total number of streamlines to draw. At 1080p
 * this gives good coverage without overwhelming the map.
 *
 * TRACE_STEPS: number of points per streamline. More steps = longer
 * traces. Each step advances by STEP_DEG degrees along the wind.
 *
 * STEP_DEG: spatial step size in degrees. Smaller = smoother curves
 * but shorter visible traces (need more steps to cover distance).
 */
/*
 * MAX_STREAMLINES: seed attempts. Most will produce a streamline
 * since we accept nearly everything above dead calm. At 1080p
 * 8000 gives good coverage; the one-time render cost is ~2-3s
 * on a Pi Zero W (cached, so zero per-frame cost).
 *
 * TRACE_STEPS_MAX: maximum steps for the strongest winds.
 * Lighter winds get fewer steps (shorter tails).
 *
 * STEP_DEG: spatial step size in degrees per step.
 */
#define MAX_STREAMLINES 8000
#define TRACE_STEPS_MIN 4     /* Light breeze — short tails    */
#define TRACE_STEPS_MAX 18    /* Storm — long dramatic tails   */
#define STEP_DEG        0.7

/*
 * Wind speed thresholds (m/s) for visual scaling.
 *
 *   < CALM:     skip entirely (dead calm, no visible wind)
 *   CALM-LIGHT: short dim traces (gentle breeze)
 *   LIGHT-MOD:  medium traces, moderate brightness
 *   MOD-STRONG: long bright traces
 *   > STRONG:   maximum length and brightness (storms)
 *
 * The Beaufort scale reference:
 *   1-3 m/s  = light breeze (Beaufort 2-3)
 *   4-8 m/s  = moderate breeze (Beaufort 3-4)
 *   8-14 m/s = fresh to strong (Beaufort 5-6)
 *   14-25 m/s = near gale to storm (Beaufort 7-10)
 *   25+ m/s  = violent storm/hurricane (Beaufort 11-12)
 */
#define WIND_CALM    0.5   /* m/s — below this, skip           */
#define WIND_STRONG  20.0  /* m/s — full brightness at ~45 mph */

/* ── Cached surface ──────────────────────────────────────────── */

/*
 * The streamlines are expensive to compute (4000 traces x 12 steps
 * = 48,000 line segments). We cache the result on an off-screen
 * surface and only recompute when new wind data arrives.
 */
static cairo_surface_t *wind_cache = NULL;
static int wind_cache_w = 0, wind_cache_h = 0;
static time_t wind_cache_ref = 0;  /* ref_time of cached data */

/* Simple pseudo-random number generator — deterministic so the
 * streamline pattern is stable between identical data sets.
 * Uses a basic LCG (linear congruential generator). */
static unsigned int wind_rng_state;

static void wind_seed_rng(unsigned int seed)
{
    wind_rng_state = seed;
}

static unsigned int wind_rand(void)
{
    wind_rng_state = wind_rng_state * 1103515245 + 12345;
    return (wind_rng_state >> 16) & 0x7FFF;
}

static double wind_randf(void)
{
    return (double)wind_rand() / 32767.0;
}

/*
 * sample_wind - Bilinear interpolation of wind at a lat/lon point.
 *
 * Returns the interpolated U and V components. Handles wraparound
 * at the date line (longitude 359 -> 0) and clamping at the poles.
 */
static void sample_wind(const pic_wind_t *data,
                        double lat, double lon,
                        double *u_out, double *v_out)
{
    double gx, gy;
    int x0, y0, x1, y1;
    double fx, fy;

    /* Convert lat/lon to grid coordinates.
     * Grid: row 0 = 90N, row 180 = 90S, col 0 = 0E, col 359 = 359E */
    gy = (90.0 - lat);          /* 0 at north pole, 180 at south */
    gx = lon;
    if (gx < 0) gx += 360.0;
    if (gx >= 360.0) gx -= 360.0;

    /* Integer grid cell and fractional offset */
    y0 = (int)gy;
    x0 = (int)gx;
    fy = gy - y0;
    fx = gx - x0;

    /* Clamp Y to valid range */
    if (y0 < 0) { y0 = 0; fy = 0; }
    if (y0 >= PIC_WIND_NY - 1) { y0 = PIC_WIND_NY - 2; fy = 1.0; }
    y1 = y0 + 1;

    /* Wrap X for date line crossing */
    if (x0 < 0) x0 += PIC_WIND_NX;
    x1 = (x0 + 1) % PIC_WIND_NX;

    /* Bilinear interpolation */
    {
        double u00 = data->u[y0][x0], u10 = data->u[y0][x1];
        double u01 = data->u[y1][x0], u11 = data->u[y1][x1];
        double v00 = data->v[y0][x0], v10 = data->v[y0][x1];
        double v01 = data->v[y1][x0], v11 = data->v[y1][x1];

        double u_top = u00 + (u10 - u00) * fx;
        double u_bot = u01 + (u11 - u01) * fx;
        *u_out = u_top + (u_bot - u_top) * fy;

        double v_top = v00 + (v10 - v00) * fx;
        double v_bot = v01 + (v11 - v01) * fx;
        *v_out = v_top + (v_bot - v_top) * fy;
    }
}

/*
 * render_streamlines - Draw all streamlines onto a Cairo context.
 *
 * Seeds random points weighted by wind speed, traces each one
 * forward through the wind field, and draws the resulting curves
 * with a brightness gradient (white at head, transparent at tail).
 */
/*
 * speed_to_brightness - Map wind speed to head brightness.
 *
 * Light breeze: dim (0.15-0.25)
 * Moderate:     medium (0.3-0.5)
 * Strong:       bright (0.6-0.8)
 * Storm:        intense white (0.9-1.0)
 *
 * Uses a sqrt curve so light winds are visible but don't
 * overwhelm, while storms really pop.
 */
static double speed_to_brightness(double speed)
{
    double norm = speed / WIND_STRONG;
    if (norm > 1.0) norm = 1.0;
    return 0.15 + 0.75 * sqrt(norm);
}

/*
 * speed_to_steps - Map wind speed to trace length.
 *
 * Light breeze: short tails (4-6 steps)
 * Moderate:     medium (8-12 steps)
 * Strong/storm: long dramatic tails (14-18 steps)
 */
static int speed_to_steps(double speed)
{
    double norm = speed / WIND_STRONG;
    if (norm > 1.0) norm = 1.0;
    return TRACE_STEPS_MIN +
           (int)(norm * (TRACE_STEPS_MAX - TRACE_STEPS_MIN));
}

static void render_streamlines(cairo_t *cr, int width, int height,
                               const pic_wind_t *data)
{
    int s, t;
    double line_w = height / 1080.0;

    if (line_w < 0.5) line_w = 0.5;
    cairo_set_line_width(cr, line_w);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    /* Seed the RNG deterministically from the wind reference time */
    wind_seed_rng((unsigned int)data->ref_time);

    for (s = 0; s < MAX_STREAMLINES; s++) {
        double lat, lon;
        double u, v, speed;
        double trace_lat[TRACE_STEPS_MAX + 1];
        double trace_lon[TRACE_STEPS_MAX + 1];
        int valid_steps, max_steps;
        double max_trace_speed;
        double brightness;

        /* Random seed point — uniform on sphere */
        lon = wind_randf() * 360.0 - 180.0;
        lat = asin(wind_randf() * 2.0 - 1.0) * RAD_TO_DEG;

        /* Check wind speed at seed point */
        sample_wind(data, lat, lon, &u, &v);
        speed = sqrt(u * u + v * v);

        /* Skip dead calm only — accept everything else.
         * Light winds still get short dim traces. */
        if (speed < WIND_CALM) continue;

        /* Determine trace length and brightness from speed */
        max_steps = speed_to_steps(speed);
        max_trace_speed = speed;

        /* Trace the streamline forward through the wind field */
        trace_lat[0] = lat;
        trace_lon[0] = lon;
        valid_steps = 1;

        for (t = 0; t < max_steps; t++) {
            double step_scale;

            sample_wind(data, lat, lon, &u, &v);
            speed = sqrt(u * u + v * v);
            if (speed < 0.3) break;

            step_scale = STEP_DEG / speed;
            lat += v * step_scale;
            lon += u * step_scale / cos(lat * DEG_TO_RAD + 0.001);

            if (lat > 85.0) lat = 85.0;
            if (lat < -85.0) lat = -85.0;
            if (lon > 180.0) lon -= 360.0;
            if (lon < -180.0) lon += 360.0;

            trace_lat[valid_steps] = lat;
            trace_lon[valid_steps] = lon;
            if (speed > max_trace_speed) max_trace_speed = speed;
            valid_steps++;
        }

        if (valid_steps < 3) continue;

        /* Convert trace to screen coordinates */
        {
            double sx[TRACE_STEPS_MAX + 1];
            double sy[TRACE_STEPS_MAX + 1];
            int skip = 0;

            for (t = 0; t < valid_steps; t++) {
                sx[t] = pic_lon_to_x(trace_lon[t], width);
                sy[t] = pic_lat_to_y(trace_lat[t], height);
                if (t > 0 && fabs(sx[t] - sx[t-1]) > width / 2.0)
                    skip = 1;
            }
            if (skip) continue;

            brightness = speed_to_brightness(max_trace_speed);

            /*
             * Draw as a single continuous path with a linear
             * gradient from transparent (tail) to white (head).
             *
             * Uses Catmull-Rom to cubic Bézier conversion with
             * tight tension (0.25) to avoid the overshoot/wiggle
             * that looser curves create. The result is smooth
             * flowing curves that follow the wind faithfully.
             */
            {
                cairo_pattern_t *grad;
                double tail_x = sx[0], tail_y = sy[0];
                double head_x = sx[valid_steps - 1];
                double head_y = sy[valid_steps - 1];

                cairo_new_path(cr);
                cairo_move_to(cr, sx[0], sy[0]);

                for (t = 0; t < valid_steps - 1; t++) {
                    /*
                     * Catmull-Rom tangent estimation with tight tension.
                     * Tangent at point i = (point[i+1] - point[i-1]) * tension
                     * Clamped at endpoints to avoid reaching outside the array.
                     * Tension 0.25 = tight curves that follow the data closely.
                     */
                    double tension = 0.25;
                    double t0x, t0y, t1x, t1y;
                    int p = (t > 0) ? t - 1 : 0;
                    int n = (t + 2 < valid_steps) ? t + 2 : valid_steps - 1;

                    /* Tangent at start of segment */
                    t0x = (sx[t+1] - sx[p]) * tension;
                    t0y = (sy[t+1] - sy[p]) * tension;

                    /* Tangent at end of segment */
                    t1x = (sx[n] - sx[t]) * tension;
                    t1y = (sy[n] - sy[t]) * tension;

                    cairo_curve_to(cr,
                        sx[t] + t0x,     sy[t] + t0y,      /* ctrl 1 */
                        sx[t+1] - t1x,   sy[t+1] - t1y,    /* ctrl 2 */
                        sx[t+1],         sy[t+1]);          /* end    */
                }

                /* Linear gradient: transparent tail → bright head */
                grad = cairo_pattern_create_linear(
                    tail_x, tail_y, head_x, head_y);
                cairo_pattern_add_color_stop_rgba(grad, 0.0,
                    1.0, 1.0, 1.0, 0.0);
                cairo_pattern_add_color_stop_rgba(grad, 0.4,
                    1.0, 1.0, 1.0, brightness * 0.25);
                cairo_pattern_add_color_stop_rgba(grad, 1.0,
                    1.0, 1.0, 1.0, brightness * 0.7);

                cairo_set_source(cr, grad);
                cairo_set_line_width(cr, line_w);
                cairo_stroke(cr);
                cairo_pattern_destroy(grad);
            }
        }
    }
}

/*
 * pic_layer_render_wind - Render wind streamlines on the map.
 *
 * Uses surface caching: streamlines are only recomputed when the
 * wind data reference time changes. On all other frames, the
 * cached surface is painted directly (~0ms).
 *
 * user_data points to a pic_wind_t.
 */
void pic_layer_render_wind(cairo_t *cr, int width, int height,
                           time_t now, void *user_data)
{
    pic_wind_t *data = (pic_wind_t *)user_data;
    time_t ref;
    int need_redraw = 0;

    (void)now;

    if (!data) return;

    /* Check if we have valid wind data */
    pthread_mutex_lock(&data->mutex);
    if (!data->valid) {
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    ref = data->ref_time;
    pthread_mutex_unlock(&data->mutex);

    /* Allocate or resize cache surface */
    if (!wind_cache || wind_cache_w != width || wind_cache_h != height) {
        if (wind_cache) cairo_surface_destroy(wind_cache);
        wind_cache = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(wind_cache) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "wind: cache surface alloc failed\n");
            cairo_surface_destroy(wind_cache);
            wind_cache = NULL;
            return;
        }
        wind_cache_w = width;
        wind_cache_h = height;
        need_redraw = 1;
    }

    /* Recompute streamlines if wind data changed */
    if (ref != wind_cache_ref) {
        need_redraw = 1;
    }

    if (need_redraw) {
        cairo_t *cc = cairo_create(wind_cache);

        /* Clear to transparent */
        cairo_set_operator(cc, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cc);
        cairo_set_operator(cc, CAIRO_OPERATOR_OVER);

        printf("wind: rendering %d streamlines...\n", MAX_STREAMLINES);

        /* Lock data for the duration of streamline computation.
         * This holds the mutex for ~50-200ms but the fetcher thread
         * only writes once every 2 hours, so contention is negligible. */
        pthread_mutex_lock(&data->mutex);
        render_streamlines(cc, width, height, data);
        pthread_mutex_unlock(&data->mutex);

        wind_cache_ref = ref;
        cairo_destroy(cc);
        printf("wind: streamlines cached\n");
    }

    /* Paint cached surface */
    cairo_set_source_surface(cr, wind_cache, 0, 0);
    cairo_paint(cr);
}

/*
 * pic_wind_cleanup - Free the cached wind surface.
 *
 * Called from the display shutdown path.
 */
void pic_wind_cleanup(void)
{
    if (wind_cache) {
        cairo_surface_destroy(wind_cache);
        wind_cache = NULL;
        wind_cache_w = 0;
        wind_cache_h = 0;
    }
}
