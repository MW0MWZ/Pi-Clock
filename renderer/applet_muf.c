/*
 * applet_muf.c - Maximum Usable Frequency applet
 *
 * Displays estimated MUF for different directions from the home
 * QTH using live solar data from NOAA SWPC.
 *
 * The MUF determines the highest frequency that will be reflected
 * by the ionosphere for a given path. Uses:
 *   - Live SFI from NOAA (via solar weather fetcher)
 *   - Real solar declination from date (Meeus algorithm)
 *   - Actual home QTH coordinates from config
 *   - Solar zenith angle at the path midpoint
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "solarweather.h"
#include "solar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "math_utils.h"

/* Cached QTH — re-read from config periodically.
 * qth_last_read is reset to 0 on config reload to force immediate re-read. */
static double qth_lat = 0, qth_lon = 0;
static time_t qth_last_read = 0;

/* Called by display.c on config reload to force QTH re-read */
void pic_muf_invalidate_qth(void)
{
    qth_last_read = 0;
}

/* Default SFI until first fetch completes */
static double solar_flux = 100.0;  /* written under mutex, read in render */

/*
 * read_qth - Read home QTH from config (at most once per minute).
 */
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
        if (strncmp(cl, "QTH_LON=", 8) == 0)
            qth_lon = atof(cl + 8);
    }
    fclose(cf);
}

/*
 * estimate_muf - Estimate MUF for a path from QTH to a midpoint.
 *
 * Uses the real solar position (declination + GHA) to compute the
 * solar zenith angle at the ionospheric reflection point, then
 * derives foF2 and MUF from SFI and the zenith angle.
 *
 *   sfi     - Solar flux index (from NOAA)
 *   sun     - Current solar position (declination + GHA)
 *   mid_lat - Latitude of the path midpoint (degrees)
 *   mid_lon - Longitude of the path midpoint (degrees)
 *
 * Returns estimated MUF in MHz.
 */
static double estimate_muf(double sfi, const pic_solar_position_t *sun,
                           double mid_lat, double mid_lon)
{
    double foF2, cos_chi, muf;
    double lat_rad = mid_lat * DEG_TO_RAD;
    double lon_rad = mid_lon * DEG_TO_RAD;

    /*
     * Solar zenith angle at the reflection point.
     * ha = local hour angle = GHA - longitude (east-positive)
     * cos(zenith) = sin(lat)*sin(dec) + cos(lat)*cos(dec)*cos(ha)
     */
    {
        double ha = lon_rad + sun->gha;
        cos_chi = sin(lat_rad) * sin(sun->declination) +
                  cos(lat_rad) * cos(sun->declination) * cos(ha);
    }

    /*
     * Night floor — ionosphere doesn't vanish completely at night.
     * Residual ionisation keeps some propagation possible on lower bands.
     */
    if (cos_chi < 0.05) cos_chi = 0.05;

    /*
     * Critical frequency of the F2 layer.
     *
     * Empirical relationship: foF2 scales with sqrt(SFI) and the
     * solar zenith angle. The exponent 0.6 accounts for the non-linear
     * relationship between ionisation rate and solar elevation.
     *
     *   foF2 ≈ k * sqrt(SFI) * cos(chi)^0.6
     *
     * k ≈ 0.5 gives values consistent with observed foF2 ranges
     * (3-4 MHz at night, 8-14 MHz at noon with high SFI).
     */
    foF2 = 0.5 * sqrt(sfi) * pow(cos_chi, 0.6);

    /*
     * MUF = foF2 * sec(elevation_angle)
     *
     * The obliquity factor depends on the path distance and the
     * F2 layer height (~300 km). For typical HF paths:
     *   Overhead (short):  factor ≈ 1.0
     *   ~1000 km:          factor ≈ 2.5
     *   ~2000 km:          factor ≈ 3.2
     *   ~3000 km:          factor ≈ 3.5
     *
     * We use 3.0 for the cardinal directions (~2000 km paths)
     * and 1.0 for overhead.
     */
    muf = foF2 * 3.0;

    return muf;
}

/*
 * pic_applet_render_muf - Render the MUF applet panel.
 *
 * Shows estimated MUF for 4 cardinal directions from QTH,
 * plus overhead. Uses live solar data and real QTH position.
 */
double pic_applet_render_muf(cairo_t *cr, double width,
                             time_t now, void *user_data)
{
    pic_solar_position_t sun;
    double title_fs = width / 12.0;   /* Same as all other applets */
    double fs = width / 16.0;         /* Smaller for data rows */
    double line_h = fs * 1.4;
    double total_h = title_fs * 1.5 + line_h * 5.5;

    /* Read live SFI from solar weather data */
    if (user_data) {
        pic_solar_data_t *sol = (pic_solar_data_t *)user_data;
        pthread_mutex_lock(&sol->mutex);
        if (sol->sfi > 0) solar_flux = (double)sol->sfi;
        pthread_mutex_unlock(&sol->mutex);
    }

    if (!cr) return total_h;

    /* Get real solar position (declination + GHA) for this moment */
    sun = pic_solar_position(now);

    /* Refresh cached QTH from config */
    read_qth(now);

    /* Title — consistent size across all applets */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, title_fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, title_fs);
    cairo_show_text(cr, "MUF Estimate");

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, title_fs + 4);
    cairo_line_to(cr, width, title_fs + 4);
    cairo_stroke(cr);

    /* MUF for each direction */
    {
        /*
         * Path midpoints — approximately 2000 km (~18 degrees) from QTH
         * in each cardinal direction, plus overhead (QTH itself).
         */
        static const struct {
            const char *dir;
            double lat_off;  /* Degrees latitude offset */
            double lon_off;  /* Degrees longitude offset */
            double obliquity; /* Path obliquity factor */
        } paths[] = {
            {"North",    18.0,   0.0, 3.0},
            {"East",      0.0,  25.0, 3.0},  /* 25° lon ≈ 2000km at mid-lat */
            {"South",   -18.0,   0.0, 3.0},
            {"West",      0.0, -25.0, 3.0},
            {"Overhead",  0.0,   0.0, 1.0},
        };
        int i;

        cairo_select_font_face(cr, "monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 1.0);

        for (i = 0; i < 5; i++) {
            double y = title_fs + line_h * (i + 1) + 4;
            double mid_lat = qth_lat + paths[i].lat_off;
            double mid_lon = qth_lon + paths[i].lon_off;
            double muf;
            double r, g, b;

            muf = estimate_muf(solar_flux, &sun, mid_lat, mid_lon);

            /* Adjust for overhead path (obliquity = 1.0) */
            if (paths[i].obliquity < 2.0) {
                muf = muf / 3.0 * paths[i].obliquity;
            }

            /* Colour: green > 14 MHz, yellow > 7 MHz, red below */
            if (muf >= 14.0) {
                r = 0.2; g = 0.9; b = 0.3;
            } else if (muf >= 7.0) {
                r = 1.0; g = 0.8; b = 0.2;
            } else {
                r = 0.9; g = 0.3; b = 0.2;
            }

            /* Label left, value right-aligned */
            {
                char val[16];
                cairo_text_extents_t ext;

                snprintf(val, sizeof(val), "%.1f MHz", muf);

                cairo_set_source_rgba(cr, r, g, b, 0.8);
                cairo_move_to(cr, 0, y);
                cairo_show_text(cr, paths[i].dir);

                cairo_text_extents(cr, val, &ext);
                cairo_move_to(cr, width - ext.x_advance - 2, y);
                cairo_show_text(cr, val);
            }
        }
    }

    return total_h;
}
