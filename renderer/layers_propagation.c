/*
 * layers_propagation.c - HF propagation heat map overlay
 *
 * Draws a translucent heat map showing signal reliability from the
 * home QTH to every point on the map for the current HF band.
 * Cycles through the amateur HF bands (80m–10m). Each grid cell
 * is colored by the VOACAP colour scheme — naturally showing skip
 * zones as gaps and different band characteristics.
 *
 * Uses the same ionospheric model as the propagation applet.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "layer.h"
#include "display.h"
#include "solar.h"
#include "solarweather.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "math_utils.h"
#include "propagation.h"
#include "dxspot.h"

/* ── Constants ─────────────────────────────────────────────────── */

/* RE and MAX_HOP_KM defined in propagation.h as PIC_EARTH_RADIUS_KM
 * and PIC_MAX_HOP_KM — use local aliases for readability. */
#define RE          PIC_EARTH_RADIUS_KM
#define MAX_HOP_KM  PIC_MAX_HOP_KM

/* Grid resolution — 2-degree cells. At 4K this gives ~21x24 pixel
 * cells which are small enough to look smooth. */
#define GRID_W 180
#define GRID_H  90

/* Propagation bands — 8 HF bands mapped to the DX cluster band
 * indices so we can reuse the same colours via pic_band_colour(). */
#define N_BANDS 8
static const double band_freq[N_BANDS] = {
    3.6, 7.1, 10.12, 14.1, 18.1, 21.2, 24.9, 28.4
};
/* Map propagation band index to DX cluster band index */
static const int band_dx_idx[N_BANDS] = {
    1, 3, 4, 5, 6, 7, 8, 9
};

/* Band cycle interval */
#define BAND_CYCLE_SECS 10

/* Propagation math is in propagation.c — shared with applet_voacap.c */

/*
 * Surface cache for the heat map grid — written and read exclusively
 * on the main render thread, no mutex needed. See State section
 * comments for design rationale.
 */
static cairo_surface_t *grid_cache_surface = NULL;
static int grid_cache_w = 0, grid_cache_h = 0;
static int grid_cache_dirty = 1;  /* 1 = must redraw; cleared after paint */

/* ── Cached config — re-read at most every 60 seconds ──────────── */

static double cfg_qth_lat = 0, cfg_qth_lon = 0, cfg_center_lon = 0;
static double cfg_antenna_gain = 0.0;
static unsigned int cfg_band_mask = 0xFF;
static time_t cfg_last_read = 0;

/* Called by display.c on config reload to force immediate re-read */
void pic_prop_invalidate_config(void)
{
    cfg_last_read = 0;
    grid_cache_dirty = 1;
}

static void read_prop_config(time_t now)
{
    FILE *cf;
    char cl[256];

    if (now - cfg_last_read < 60) return;
    cfg_last_read = now;

    cf = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (!cf) return;

    while (fgets(cl, sizeof(cl), cf)) {
        if (strncmp(cl, "QTH_LAT=", 8) == 0) cfg_qth_lat = atof(cl + 8);
        if (strncmp(cl, "QTH_LON=", 8) == 0) cfg_qth_lon = atof(cl + 8);
        if (strncmp(cl, "CENTER_LON=", 11) == 0) cfg_center_lon = atof(cl + 11);
        if (strncmp(cl, "ANTENNA_GAIN=", 13) == 0) cfg_antenna_gain = atof(cl + 13);
        if (strncmp(cl, "PROP_BANDS=", 11) == 0) {
            unsigned int m = 0;
            if (sscanf(cl + 11, "%x", &m) == 1) cfg_band_mask = m;
        }
    }
    fclose(cf);
}

/* ── State ─────────────────────────────────────────────────────── */

static float  prop_grid[GRID_H][GRID_W];
static int    current_band = 3;  /* Start on 20m */
static time_t last_band_cycle = 0;
static int    last_computed_band = -1;
static time_t last_computed_time = 0;

/* ── Compute the reliability grid ──────────────────────────────── */

static void compute_prop_grid(double qth_lat, double qth_lon,
                              double sfi, double kp_val,
                              double ant_gain_dbi,
                              int band_idx, time_t now)
{
    double ssn = (sfi - 67.0) / 1.61;
    double qth_lat_r = qth_lat * DEG_TO_RAD;
    double qth_lon_r = qth_lon * DEG_TO_RAD;
    double freq = band_freq[band_idx];
    pic_solar_position_t sun;
    int gx, gy;

    if (ssn < 0.0) ssn = 0.0;
    if (ssn > 300.0) ssn = 300.0;

    sun = pic_solar_position(now);

    for (gy = 0; gy < GRID_H; gy++) {
        double pt_lat = (90.0 - (double)gy * 180.0 / (GRID_H - 1)) * DEG_TO_RAD;

        for (gx = 0; gx < GRID_W; gx++) {
            double pt_lon = (-180.0 + (double)gx * 360.0 / (GRID_W - 1)) * DEG_TO_RAD;

            /* Great-circle distance from QTH */
            double dlat = pt_lat - qth_lat_r;
            double dlon = pt_lon - qth_lon_r;
            double a = sin(dlat / 2) * sin(dlat / 2) +
                       cos(qth_lat_r) * cos(pt_lat) *
                       sin(dlon / 2) * sin(dlon / 2);
            double dist_km = 2.0 * RE * asin(sqrt(a));

            /* Very short paths — always open */
            if (dist_km < 50.0) {
                prop_grid[gy][gx] = 0.85f;
                continue;
            }

            /* Hops */
            int hops = (int)ceil(dist_km / MAX_HOP_KM);
            if (hops < 1) hops = 1;
            double hop_km = dist_km / hops;

            /* Great-circle midpoint for ionospheric reflection */
            double mid_lat, mid_lon;
            pic_gc_midpoint(qth_lat_r, qth_lon_r, pt_lat, pt_lon,
                        &mid_lat, &mid_lon);

            /* Solar zenith at midpoint */
            double ha = mid_lon + sun.gha;
            double cos_chi = sin(mid_lat) * sin(sun.declination) +
                             cos(mid_lat) * cos(sun.declination) * cos(ha);
            double cz = cos_chi > 0.0 ? cos_chi : 0.0;

            /* Propagation prediction */
            double foF2 = pic_foF2_estimate(mid_lat, cz, ssn);
            double M = pic_muf_m_factor(hop_km);
            double muf = pic_kp_degrade(foF2 * M, mid_lat, kp_val);
            double luf = pic_calc_luf(cz, ssn, hops, M);
            double rel = pic_band_reliability(freq, muf, luf);

            /* Multi-hop degradation — 65% loss per additional bounce.
             * Ground reflection, scattering, and defocusing make
             * multi-hop very lossy for a 100W dipole.
             * 1 hop = 100%, 2 = 35%, 3 = 12%, 4 = 4%. */
            if (hops > 1) {
                int extra = hops - 1;
                double hop_loss = 1.0;
                while (extra-- > 0) hop_loss *= 0.35;
                rel *= hop_loss;
            }

            /* 80m additional penalty — real-world 80m dipoles are
             * compromised (most hams can't fit a full-size 40m dipole),
             * ground losses are higher, and noise floor is severe. */
            if (freq < 5.0) rel *= 0.45;

            /* Antenna gain factor — dBi converted to linear.
             *
             * A -10 dB reality offset is baked in so the dashboard's
             * "0 dBi" corresponds to a -10 dB effective gain. The
             * default 100 W dipole + real-world-losses model (see
             * propagation.c threshold comment) proved ~10 dB too
             * optimistic compared to on-air observation; this
             * cosmetic shift keeps the familiar -12..+12 dashboard
             * slider while producing a propagation map that
             * matches what operators actually hear.
             *
             * Applied as a power ratio adjustment to reliability.
             * sqrt() because the receive-side gain affects only
             * half of the link budget. */
            {
                double effective_dbi = ant_gain_dbi - 10.0;
                double gain_linear = pow(10.0, effective_dbi / 10.0);
                rel *= (gain_linear > 0.01) ? sqrt(gain_linear) : 0.0;
            }

            if (rel > 1.0) rel = 1.0;
            if (rel < 0.0) rel = 0.0;
            prop_grid[gy][gx] = (float)rel;
        }
    }

    last_computed_band = band_idx;
    last_computed_time = now;
    grid_cache_dirty = 1;  /* Force rectangle redraw on next render */
}

/* ── Layer render function ─────────────────────────────────────── */

void pic_layer_render_propagation(cairo_t *cr, int width, int height,
                                  time_t now, void *user_data)
{
    pic_solar_data_t *sol = (pic_solar_data_t *)user_data;
    double sfi = 100.0, kp = 0.0;
    int gx, gy;

    if (!cr || !sol) return;

    /* Read solar data */
    pthread_mutex_lock(&sol->mutex);
    if (sol->sfi > 0) sfi = (double)sol->sfi;
    kp = sol->kp;
    pthread_mutex_unlock(&sol->mutex);

    /* Re-read config at most every 60 seconds */
    read_prop_config(now);

    /* Cycle through bands — skip bands with no propagation */
    if (now - last_band_cycle >= BAND_CYCLE_SECS) {
        int tries, found = 0;
        for (tries = 0; tries < N_BANDS; tries++) {
            current_band = (current_band + 1) % N_BANDS;

            /* Skip bands the user has disabled */
            if (!(cfg_band_mask & (1u << current_band))) continue;

            compute_prop_grid(cfg_qth_lat, cfg_qth_lon, sfi, kp, cfg_antenna_gain, current_band, now);

            /* Check if any cell has meaningful reliability */
            int has_data = 0, ck;
            for (ck = 0; ck < GRID_W * GRID_H && !has_data; ck++) {
                if (((float *)prop_grid)[ck] > 0.20f) has_data = 1;
            }
            if (has_data) { found = 1; break; }
        }
        if (found) last_band_cycle = now;
        /* If no bands have data, clear the grid so we don't
         * display stale data from a previously enabled band. */
        if (!found) memset(prop_grid, 0, sizeof(prop_grid));
    } else if (last_computed_band != current_band ||
               now - last_computed_time > 60) {
        compute_prop_grid(cfg_qth_lat, cfg_qth_lon, sfi, kp, cfg_antenna_gain, current_band, now);
    }

    /*
     * Draw heat map using a cached surface.
     *
     * The 16,200 individual Cairo rectangles are expensive (~300ms on
     * Pi Zero W). We render them to an off-screen surface only when the
     * grid data changes (grid_cache_dirty flag). On all other frames,
     * we just paint the cached surface — a single operation (<1ms).
     *
     * The cache is invalidated when:
     *   - compute_prop_grid() runs (band change or SFI update)
     *   - The display resolution changes
     */
    {
        /* Allocate or resize cache surface if needed */
        if (!grid_cache_surface ||
            grid_cache_w != width || grid_cache_h != height) {
            if (grid_cache_surface)
                cairo_surface_destroy(grid_cache_surface);
            grid_cache_surface = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, width, height);
            if (cairo_surface_status(grid_cache_surface) != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "propagation: cache surface alloc failed\n");
                cairo_surface_destroy(grid_cache_surface);
                grid_cache_surface = NULL;
                return;
            }
            grid_cache_w = width;
            grid_cache_h = height;
            grid_cache_dirty = 1;
        }

        /* Redraw the grid to the cache only when data changed.
         * Uses bilinear scaling: render to a tiny GRID_W x GRID_H
         * surface, then scale up for smooth, non-blocky edges. */
        if (grid_cache_dirty) {
            cairo_surface_t *grid_surf;
            unsigned int *pixels;
            int gs_stride;
            double br, bg, bb, ba;
            int center_off;

            grid_surf = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, GRID_W, GRID_H);
            if (cairo_surface_status(grid_surf) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(grid_surf);
            } else {
                cairo_surface_flush(grid_surf);
                pixels = (unsigned int *)cairo_image_surface_get_data(grid_surf);
                gs_stride = cairo_image_surface_get_stride(grid_surf) / 4;

                pic_band_colour(band_dx_idx[current_band], &br, &bg, &bb, &ba);

                /* Compute center_lon offset in grid columns */
                center_off = (int)((cfg_center_lon + 180.0) * GRID_W / 360.0);
                /* Clamp to valid range to avoid negative array index */
                while (center_off < 0) center_off += GRID_W;
                center_off = center_off % GRID_W;

                for (gy = 0; gy < GRID_H; gy++) {
                    for (gx = 0; gx < GRID_W; gx++) {
                        /* Map display column to data column with center_lon */
                        int src_gx = (gx + center_off) % GRID_W;
                        double rel = prop_grid[gy][src_gx];
                        unsigned int ia, ir, ig, ib;

                        if (rel < 0.10) {
                            pixels[gy * gs_stride + gx] = 0;
                            continue;
                        }

                        double alpha = 0.15 + rel * 0.45;

                        /* Pre-multiply alpha for ARGB32 */
                        ia = (unsigned int)(alpha * 255);
                        ir = (unsigned int)(br * alpha * 255);
                        ig = (unsigned int)(bg * alpha * 255);
                        ib = (unsigned int)(bb * alpha * 255);

                        pixels[gy * gs_stride + gx] =
                            (ia << 24) | (ir << 16) | (ig << 8) | ib;
                    }
                }

                cairo_surface_mark_dirty(grid_surf);

                /* Scale up with bilinear filtering */
                {
                    cairo_t *cc = cairo_create(grid_cache_surface);
                    cairo_set_operator(cc, CAIRO_OPERATOR_CLEAR);
                    cairo_paint(cc);
                    cairo_set_operator(cc, CAIRO_OPERATOR_OVER);

                    cairo_scale(cc,
                                (double)width / GRID_W,
                                (double)height / GRID_H);
                    cairo_set_source_surface(cc, grid_surf, 0, 0);
                    cairo_pattern_set_filter(cairo_get_source(cc),
                                             CAIRO_FILTER_BILINEAR);
                    cairo_paint(cc);
                    cairo_destroy(cc);
                }

                cairo_surface_destroy(grid_surf);
                grid_cache_dirty = 0;
            }
        }

        /* Paint the cached grid surface onto the layer */
        if (grid_cache_surface) {
            cairo_set_source_surface(cr, grid_cache_surface, 0, 0);
            cairo_paint(cr);
        }
    }

}

/*
 * pic_layer_render_propagation_legend - Draw the propagation band legend.
 *
 * Separated from the main render so it's drawn post-composite
 * at full opacity, independent of layer opacity.
 */
void pic_layer_render_propagation_legend(cairo_t *cr, int width, int height,
                                         time_t now, void *user_data)
{
    double legend_fs = height / 80.0;
    double dot_r = legend_fs * 0.35;
    double dot_gap = 4;
    double item_gap = legend_fs * 0.8;
    double bg_pad = legend_fs * 0.5;
    const char *title = "Propagation";
    double br, bg, bb, ba;
    cairo_text_extents_t title_ext, band_ext;
    const char *bname;
    double total_w, bg_h, gap, ref_bottom, lx, ly, cx;

    (void)now;
    (void)user_data;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, legend_fs * 0.75);

    cairo_text_extents(cr, title, &title_ext);

    bname = pic_band_name(band_dx_idx[current_band]);
    cairo_text_extents(cr, bname, &band_ext);

    total_w = title_ext.x_advance + item_gap +
              dot_r * 2 + dot_gap + band_ext.x_advance;

    bg_h = legend_fs * 1.2 + bg_pad * 2;
    gap = height / 160.0;

    if (pic_ticker_bar_top > 0) {
        ref_bottom = pic_ticker_bar_top - gap - bg_h - gap;
    } else {
        ref_bottom = height - height / 80.0 - bg_h - gap;
    }
    lx = (width - total_w) / 2.0;
    ly = ref_bottom - bg_h + bg_pad + dot_r;

    /* Background pill */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.05, 0.7);
    {
        double bx = lx - bg_pad;
        double by = ly - bg_pad - dot_r;
        double bw = total_w + bg_pad * 2;
        double cr_r = bg_pad;
        cairo_move_to(cr, bx + cr_r, by);
        cairo_line_to(cr, bx + bw - cr_r, by);
        cairo_arc(cr, bx + bw - cr_r, by + cr_r, cr_r, -PI/2, 0);
        cairo_line_to(cr, bx + bw, by + bg_h - cr_r);
        cairo_arc(cr, bx + bw - cr_r, by + bg_h - cr_r, cr_r, 0, PI/2);
        cairo_line_to(cr, bx + cr_r, by + bg_h);
        cairo_arc(cr, bx + cr_r, by + bg_h - cr_r, cr_r, PI/2, PI);
        cairo_line_to(cr, bx, by + cr_r);
        cairo_arc(cr, bx + cr_r, by + cr_r, cr_r, PI, 3*PI/2);
        cairo_close_path(cr);
    }
    cairo_fill(cr);

    cx = lx;

    /* Title */
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 0.9);
    cairo_move_to(cr, cx, ly + legend_fs * 0.25);
    cairo_show_text(cr, title);
    cx += title_ext.x_advance + item_gap;

    /* Band dot + name */
    pic_band_colour(band_dx_idx[current_band], &br, &bg, &bb, &ba);
    cairo_arc(cr, cx + dot_r, ly, dot_r, 0, 2 * PI);
    cairo_set_source_rgba(cr, br, bg, bb, 0.9);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, br, bg, bb, 0.85);
    cairo_move_to(cr, cx + dot_r * 2 + dot_gap, ly + legend_fs * 0.25);
    cairo_show_text(cr, bname);
}

/*
 * pic_prop_cleanup - Free the cached grid surface.
 *
 * Called from the display shutdown path to avoid a fixed 8-33 MB
 * leak on a 512 MB Pi Zero W running 24/7.
 */
void pic_prop_cleanup(void)
{
    if (grid_cache_surface) {
        cairo_surface_destroy(grid_cache_surface);
        grid_cache_surface = NULL;
        grid_cache_w = 0;
        grid_cache_h = 0;
    }
}
