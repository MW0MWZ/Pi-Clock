/*
 * applet_voacap.c - HF propagation prediction applet
 *
 * Displays a VOACAP-style band-by-hour heat map showing predicted
 * signal reliability for each amateur HF band across 24 UTC hours.
 * Predictions are computed locally using simplified ionospheric
 * models — no external API calls needed (works fully offline).
 *
 * The applet cycles through multiple distance brackets, from NVIS
 * (local) to antipodal paths. Each distance computes the path MUF,
 * LUF, and per-band reliability using:
 *   - Live SFI from NOAA SWPC (via solar weather fetcher)
 *   - Solar zenith angle at path midpoint (Meeus algorithm)
 *   - F2 layer critical frequency (latitude/SSN empirical model)
 *   - D-layer absorption (ITU-R P.533 simplified)
 *   - Path geometry (multi-hop obliquity factor)
 *
 * Based on MINIMUF 3.5 (McNamara 1982) and ITU-R P.533-14.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "solarweather.h"
#include "solar.h"
#include "propagation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "math_utils.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define RE          PIC_EARTH_RADIUS_KM
#define MAX_HOP_KM  PIC_MAX_HOP_KM

/* Ham bands — centre frequencies in MHz */
#define N_BANDS 8
static const double band_freq[N_BANDS] = {
    3.6, 7.1, 10.12, 14.1, 18.1, 21.2, 24.9, 28.4
};
static const char *band_label[N_BANDS] = {
    "80", "40", "30", "20", "17", "15", "12", "10"
};

/* Distance brackets — from local NVIS to antipodal.
 * Distances in km, labels shown on the chart title.
 * Computed for 4 cardinal bearings and averaged. */
#define N_DISTANCES 8
static const struct {
    double km;
    const char *name;
} distances[N_DISTANCES] = {
    {   500, "NVIS"      },
    {  1200, "Near"      },
    {  2400, "Medium"    },
    {  5500, "Long"      },
    {  7200, "Very Long" },
    {  8700, "DX"        },
    {  9600, "Far DX"    },
    { 17000, "Antipodal" },
};

/* Cycle interval — seconds between distance chart changes */
#define CYCLE_SECS 10

/* ── State ─────────────────────────────────────────────────────── */

/* Cached QTH from config (re-read periodically) */
static double qth_lat = 0, qth_lon = 0;
static time_t qth_last_read = 0;

/* Live solar data */
static double solar_flux = 100.0;
static double kp_index   = 0.0;
static double antenna_gain_dbi = 0.0;

/* Current distance index and cycle timer */
static int    current_dist = 0;
static time_t last_cycle   = 0;

/* Pre-computed grid: reliability values [hour][band] */
static float  grid[24][N_BANDS];
static double grid_muf = 0, grid_luf = 0;  /* For current hour */
static int    grid_valid = 0;
static int    grid_dist  = -1;              /* Which distance was computed */
static time_t grid_time  = 0;              /* When grid was last computed */

/* Called by display.c on config reload to force QTH re-read */
void pic_voacap_invalidate_qth(void)
{
    qth_last_read = 0;
    grid_valid = 0;
}

/* ── QTH reader ────────────────────────────────────────────────── */

static void read_qth(time_t now)
{
    FILE *cf;
    char cl[256];

    if (now - qth_last_read < 60) return;
    qth_last_read = now;

    cf = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (!cf) return;

    while (fgets(cl, sizeof(cl), cf)) {
        if (strncmp(cl, "QTH_LAT=", 8) == 0)
            qth_lat = atof(cl + 8);
        if (strncmp(cl, "ANTENNA_GAIN=", 13) == 0)
            antenna_gain_dbi = atof(cl + 13);
        if (strncmp(cl, "QTH_LON=", 8) == 0)
            qth_lon = atof(cl + 8);
    }
    fclose(cf);
}

/* Propagation math (foF2, MUF, LUF, reliability, kp_degrade) is in
 * propagation.c — shared single implementation with layers_propagation.c. */

/*
 * cos_zenith - Cosine of solar zenith angle at a point.
 *
 * Uses the pre-computed solar position (declination + GHA) from
 * solar.c rather than recomputing from scratch.
 *
 *   lat_r - Latitude in radians
 *   lon_r - Longitude in radians
 *   sun   - Current solar position
 */
static double cos_zenith(double lat_r, double lon_r,
                         const pic_solar_position_t *sun)
{
    double ha = lon_r + sun->gha;
    return sin(lat_r) * sin(sun->declination) +
           cos(lat_r) * cos(sun->declination) * cos(ha);
}


/*
 * compute_grid - Calculate the full 24-hour x N_BANDS reliability grid.
 *
 * For the given distance, computes propagation for 4 cardinal bearings
 * from QTH and averages the reliability. This gives a bearing-independent
 * prediction for each distance bracket.
 */
static void compute_grid(double dist_km, time_t now)
{
    double ssn = (solar_flux - 67.0) / 1.61;
    int hops, b, h, d;
    double hop_km;
    struct tm local_buf, *local_p;

    if (ssn < 0.0) ssn = 0.0;
    if (ssn > 300.0) ssn = 300.0;

    hops = (int)ceil(dist_km / MAX_HOP_KM);
    if (hops < 1) hops = 1;
    hop_km = dist_km / hops;

    /* Use local time so the chart columns match the QTH's clock.
     * The solar position math still uses UTC internally (time_t). */
    local_p = localtime(&now);
    if (!local_p) return;
    local_buf = *local_p;  /* Copy — localtime returns a static buffer */

    /* Bearing offsets: N, E, S, W.
     * Convert distance to angular offset (degrees). */
    {
        double ang_deg = (dist_km / RE) * RAD_TO_DEG;
        /* Longitude offset scaled by cos(latitude) for E/W paths */
        double lon_scale = cos(qth_lat * DEG_TO_RAD);
        if (lon_scale < 0.1) lon_scale = 0.1;  /* Avoid divide-by-zero near poles */

        double lat_off = ang_deg * DEG_TO_RAD / 2.0;  /* Midpoint = half distance */
        double lon_off = (ang_deg / lon_scale) * DEG_TO_RAD / 2.0;

        /* Midpoints for 4 bearings (lat, lon in radians) */
        double mid[4][2] = {
            { qth_lat * DEG_TO_RAD + lat_off,  qth_lon * DEG_TO_RAD },           /* North */
            { qth_lat * DEG_TO_RAD,             qth_lon * DEG_TO_RAD + lon_off }, /* East  */
            { qth_lat * DEG_TO_RAD - lat_off,   qth_lon * DEG_TO_RAD },           /* South */
            { qth_lat * DEG_TO_RAD,             qth_lon * DEG_TO_RAD - lon_off }, /* West  */
        };

        /* Clamp midpoint latitudes to +/-85 degrees */
        for (d = 0; d < 4; d++) {
            if (mid[d][0] > 85.0 * DEG_TO_RAD) mid[d][0] = 85.0 * DEG_TO_RAD;
            if (mid[d][0] < -85.0 * DEG_TO_RAD) mid[d][0] = -85.0 * DEG_TO_RAD;
        }

        for (h = 0; h < 24; h++) {
            /* Solar position for this hour — advance from current time.
             * We compute for today, hour by hour. */
            time_t t_h = now - (local_buf.tm_hour * 3600 + local_buf.tm_min * 60 +
                          local_buf.tm_sec) + h * 3600 + 1800; /* Centre of hour */
            pic_solar_position_t sun = pic_solar_position(t_h);

            for (b = 0; b < N_BANDS; b++) {
                double total_rel = 0.0;

                for (d = 0; d < 4; d++) {
                    double cz = cos_zenith(mid[d][0], mid[d][1], &sun);
                    double cz_clamp = cz > 0.0 ? cz : 0.0;
                    double foF2 = pic_foF2_estimate(mid[d][0], cz_clamp, ssn);
                    double M    = pic_muf_m_factor(hop_km);
                    double muf  = foF2 * M;

                    /* Apply Kp degradation */
                    muf = pic_kp_degrade(muf, mid[d][0], kp_index);

                    double luf = pic_calc_luf(cz_clamp, ssn, hops, M);

                    double rel = pic_band_reliability(band_freq[b], muf, luf);

                    /* Apply antenna gain factor */
                    if (antenna_gain_dbi != 0.0) {
                        double gl = pow(10.0, antenna_gain_dbi / 10.0);
                        rel *= (gl > 0.01) ? sqrt(gl) : 0.0;
                        if (rel > 1.0) rel = 1.0;
                    }

                    total_rel += rel;
                }

                grid[h][b] = (float)(total_rel / 4.0);
            }

            /* Store MUF/LUF for the current hour (average of 4 bearings) */
            if (h == local_buf.tm_hour) {
                pic_solar_position_t sun_now = pic_solar_position(now);
                double muf_sum = 0, luf_sum = 0;
                for (d = 0; d < 4; d++) {
                    double cz = cos_zenith(mid[d][0], mid[d][1], &sun_now);
                    double cz_clamp = cz > 0.0 ? cz : 0.0;
                    double foF2 = pic_foF2_estimate(mid[d][0], cz_clamp, ssn);
                    double M = pic_muf_m_factor(hop_km);
                    muf_sum += pic_kp_degrade(foF2 * M,
                                          mid[d][0], kp_index);
                    luf_sum += pic_calc_luf(cz_clamp, ssn, hops, M);
                }
                grid_muf = muf_sum / 4.0;
                grid_luf = luf_sum / 4.0;
            }
        }
    }

    grid_valid = 1;
    grid_dist  = current_dist;
    grid_time  = now;
}

/* ── Colour mapping ────────────────────────────────────────────── */

/*
 * Map reliability (0.0–1.0) to a colour for the heat map.
 *
 * Uses the standard VOACAP colour scheme: a smooth gradient from
 * dark/black (closed) through dark red, red, orange, yellow,
 * yellow-green to bright green (fully open). Linearly interpolated
 * between anchor points for smooth transitions.
 */
static void rel_colour(double rel, double *r, double *g, double *b)
{
    /* VOACAP-style colour stops: {reliability, R, G, B}
     * Matches the voacap.com colour bar (left to right):
     * white → cyan → green → yellow → orange → red
     * 0% = no propagation (white), 100% = best (red) */
    static const double stops[][4] = {
        { 0.00,  0.85, 0.85, 0.85 },  /* White/light grey: closed    */
        { 0.10,  0.45, 0.85, 0.85 },  /* Light cyan                  */
        { 0.20,  0.25, 0.80, 0.80 },  /* Cyan                        */
        { 0.30,  0.25, 0.82, 0.55 },  /* Teal green                  */
        { 0.40,  0.30, 0.80, 0.30 },  /* Green                       */
        { 0.50,  0.55, 0.82, 0.25 },  /* Yellow-green                */
        { 0.60,  0.80, 0.85, 0.20 },  /* Yellow                      */
        { 0.70,  0.92, 0.82, 0.18 },  /* Yellow-orange               */
        { 0.80,  0.95, 0.65, 0.15 },  /* Orange                      */
        { 0.90,  0.95, 0.40, 0.15 },  /* Orange-red                  */
        { 1.00,  0.92, 0.20, 0.15 },  /* Red: fully open             */
    };
    int n = (int)(sizeof(stops) / sizeof(stops[0]));
    int i;
    double t;

    if (rel <= 0.0) { *r = stops[0][1]; *g = stops[0][2]; *b = stops[0][3]; return; }
    if (rel >= 1.0) { *r = stops[n-1][1]; *g = stops[n-1][2]; *b = stops[n-1][3]; return; }

    /* Find the two stops that bracket this reliability value */
    for (i = 1; i < n; i++) {
        if (rel <= stops[i][0]) {
            t = (rel - stops[i-1][0]) / (stops[i][0] - stops[i-1][0]);
            *r = stops[i-1][1] + t * (stops[i][1] - stops[i-1][1]);
            *g = stops[i-1][2] + t * (stops[i][2] - stops[i-1][2]);
            *b = stops[i-1][3] + t * (stops[i][3] - stops[i-1][3]);
            return;
        }
    }
    *r = stops[n-1][1]; *g = stops[n-1][2]; *b = stops[n-1][3];
}

/* ── Render ────────────────────────────────────────────────────── */

double pic_applet_render_voacap(cairo_t *cr, double width,
                                time_t now, void *user_data)
{
    double title_fs = width / 12.0;
    double fs       = width / 16.0;   /* Data text (MUF/LUF, distance) */
    double grid_fs  = width / 20.0;   /* Grid labels (bands, hours) */
    int    h, b;

    /* Grid layout — tight left margin for band labels, rest for grid.
     * The grid is left-biased: band labels are narrow. */
    double label_w  = grid_fs * 2.2;       /* Tight space for "80" etc */
    double grid_w   = width - label_w;     /* Remaining for 24 cols */
    double cell_w   = grid_w / 24.0;
    double cell_h   = grid_fs * 1.5;
    double grid_top = title_fs * 1.5 + fs * 1.5;  /* Below title + MUF/LUF */
    double hour_h   = grid_fs * 1.3;      /* Height for hour labels */
    double dist_h   = fs * 3.0;           /* Two lines for distance info */
    double total_h  = grid_top + cell_h * N_BANDS + hour_h + dist_h;

    struct tm local_render, *local_rp;
    local_rp = localtime(&now);
    if (!local_rp) return total_h;
    local_render = *local_rp;

    if (!cr) return total_h;

    /* Read solar data — only on actual render, not height probes */
    if (user_data) {
        pic_solar_data_t *sol = (pic_solar_data_t *)user_data;
        pthread_mutex_lock(&sol->mutex);
        if (sol->sfi > 0) solar_flux = (double)sol->sfi;
        kp_index = sol->kp;
        pthread_mutex_unlock(&sol->mutex);
    }

    /* Cycle through distances */
    if (now - last_cycle >= CYCLE_SECS) {
        current_dist = (current_dist + 1) % N_DISTANCES;
        last_cycle = now;
        grid_valid = 0;  /* Force recompute for new distance */
    }

    /* Refresh QTH from config */
    read_qth(now);

    /* Recompute grid if distance changed or data is stale (> 5 min) */
    if (!grid_valid || grid_dist != current_dist ||
        now - grid_time > 300) {
        compute_grid(distances[current_dist].km, now);
    }

    /* ── Title ──────────────────────────────────────────────── */
    {
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, title_fs);
        cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
        cairo_move_to(cr, 0, title_fs);
        cairo_show_text(cr, "Propagation");
    }

    /* Separator under title */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, title_fs + 4);
    cairo_line_to(cr, width, title_fs + 4);
    cairo_stroke(cr);

    /* ── MUF / LUF summary ─────────────────────────────────── */
    {
        char info[64];
        snprintf(info, sizeof(info), "MUF %.1f MHz   LUF %.1f MHz",
                 grid_muf, grid_luf);

        cairo_select_font_face(cr, "monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.7, 0.7, 0.8, 0.8);
        cairo_move_to(cr, 0, title_fs + 4 + fs * 1.2);
        cairo_show_text(cr, info);
    }

    /* ── Heat map grid ─────────────────────────────────────── */
    cairo_set_font_size(cr, grid_fs);

    /* Band labels (Y axis, right-aligned before the grid).
     * Rendered top-to-bottom: 10m at top, 80m at bottom.
     * band_freq[] is ordered low→high (80,40,...,10) so we
     * flip the row: screen row 0 = band index N_BANDS-1. */
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    for (b = 0; b < N_BANDS; b++) {
        int row = N_BANDS - 1 - b;  /* 10m (index 7) at row 0 */
        double y = grid_top + cell_h * row;
        cairo_text_extents_t ext;

        cairo_text_extents(cr, band_label[b], &ext);
        cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.9);
        cairo_move_to(cr, label_w - ext.width - 4,
                      y + cell_h * 0.5 + ext.height * 0.4);
        cairo_show_text(cr, band_label[b]);
    }

    /* Grid cells — same flip: screen row = N_BANDS-1-b */
    for (h = 0; h < 24; h++) {
        double x = label_w + cell_w * h;

        for (b = 0; b < N_BANDS; b++) {
            int row = N_BANDS - 1 - b;
            double y = grid_top + cell_h * row;
            double r, g, bv;

            /* Skip cells with no meaningful propagation — leave
             * transparent rather than painting white/grey. Below
             * 5% reliability is indistinguishable from noise. */
            if (grid[h][b] < 0.05) continue;

            rel_colour(grid[h][b], &r, &g, &bv);
            cairo_set_source_rgba(cr, r, g, bv, 0.85);
            cairo_rectangle(cr, x + 0.5, y + 0.5,
                           cell_w - 1.0, cell_h - 1.0);
            cairo_fill(cr);
        }

        /* Current hour — thin vertical line at the centre of the cell */
        if (h == local_render.tm_hour) {
            double cx = x + cell_w / 2.0;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cx, grid_top - 2);
            cairo_line_to(cr, cx, grid_top + cell_h * N_BANDS + 2);
            cairo_stroke(cr);
        }
    }

    /* Grid border */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_rectangle(cr, label_w, grid_top, grid_w, cell_h * N_BANDS);
    cairo_stroke(cr);

    /* Hour labels (X axis) — every 3 hours, 0–23 */
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, grid_fs * 0.85);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.7, 0.8);
    for (h = 0; h < 24; h += 3) {
        char lbl[4];
        cairo_text_extents_t ext;
        double x = label_w + cell_w * h + cell_w * 0.5;

        snprintf(lbl, sizeof(lbl), "%d", h);
        cairo_text_extents(cr, lbl, &ext);
        cairo_move_to(cr, x - ext.width / 2.0,
                      grid_top + cell_h * N_BANDS + grid_fs * 1.0);
        cairo_show_text(cr, lbl);
    }

    /* Distance label — two lines centred at the bottom.
     * Line 1: distance name (e.g. "Far DX")
     * Line 2: distance in miles and km */
    {
        cairo_text_extents_t ext;
        double dist_y = grid_top + cell_h * N_BANDS + hour_h;
        int    miles   = (int)(distances[current_dist].km * 0.621371 + 0.5);
        int    km      = (int)(distances[current_dist].km + 0.5);
        char   dist_str[48];

        /* Line 1: name */
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_text_extents(cr, distances[current_dist].name, &ext);
        cairo_set_source_rgba(cr, 0.8, 0.75, 0.45, 0.9);
        cairo_move_to(cr, (width - ext.width) / 2.0,
                      dist_y + fs * 1.1);
        cairo_show_text(cr, distances[current_dist].name);

        /* Line 2: distance */
        snprintf(dist_str, sizeof(dist_str), "%d mi / %d km",
                 miles, km);
        cairo_select_font_face(cr, "sans-serif",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 0.85);
        cairo_text_extents(cr, dist_str, &ext);
        cairo_set_source_rgba(cr, 0.6, 0.55, 0.4, 0.8);
        cairo_move_to(cr, (width - ext.width) / 2.0,
                      dist_y + fs * 1.1 + fs * 1.2);
        cairo_show_text(cr, dist_str);
    }

    return total_h;
}
