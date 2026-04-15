/*
 * applet_solar.c - Solar weather conditions applet
 *
 * Displays current solar indices fetched from NOAA SWPC:
 *   SFI, K-index, solar wind speed, Bz, X-ray flare class.
 *
 * Colour coding:
 *   SFI:    green >=120, yellow >=90, red below (propagation quality)
 *   Kp:     green <=3, yellow <=5, red above (geomagnetic disturbance)
 *   Bz:     green >=0, yellow >=-5, red below (southward = storms)
 *   Wind:   green <=450, yellow <=600, red above
 *   X-ray:  green A/B class, yellow C class, red M/X class
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "applet.h"
#include "solarweather.h"
#include <stdio.h>
#include <string.h>

/*
 * colour_sfi - Colour code for solar flux index.
 * Higher SFI = better HF propagation.
 */
static void colour_sfi(int sfi, double *r, double *g, double *b)
{
    if (sfi >= 120)      { *r = 0.2; *g = 0.9; *b = 0.3; }
    else if (sfi >= 90)  { *r = 1.0; *g = 0.8; *b = 0.2; }
    else                 { *r = 0.9; *g = 0.3; *b = 0.2; }
}

/*
 * colour_kp - Colour code for K-index.
 * Lower Kp = quieter geomagnetic conditions = better propagation.
 */
static void colour_kp(double kp, double *r, double *g, double *b)
{
    if (kp <= 3.0)       { *r = 0.2; *g = 0.9; *b = 0.3; }
    else if (kp <= 5.0)  { *r = 1.0; *g = 0.8; *b = 0.2; }
    else                 { *r = 0.9; *g = 0.3; *b = 0.2; }
}

/*
 * colour_bz - Colour code for Bz GSM component.
 * Positive or zero = quiet. Negative = geomagnetic coupling.
 */
static void colour_bz(double bz, double *r, double *g, double *b)
{
    if (bz >= 0)         { *r = 0.2; *g = 0.9; *b = 0.3; }
    else if (bz >= -5.0) { *r = 1.0; *g = 0.8; *b = 0.2; }
    else                 { *r = 0.9; *g = 0.3; *b = 0.2; }
}

/*
 * colour_wind - Colour code for solar wind speed.
 * Below 450 km/s is nominal.
 */
static void colour_wind(int speed, double *r, double *g, double *b)
{
    if (speed <= 450)     { *r = 0.2; *g = 0.9; *b = 0.3; }
    else if (speed <= 600){ *r = 1.0; *g = 0.8; *b = 0.2; }
    else                  { *r = 0.9; *g = 0.3; *b = 0.2; }
}

/*
 * colour_xray - Colour code for X-ray flare classification.
 * A/B = quiet, C = minor, M/X = significant.
 */
static void colour_xray(const char *cls, double *r, double *g, double *b)
{
    if (cls[0] == 'M' || cls[0] == 'X') {
        *r = 0.9; *g = 0.3; *b = 0.2;
    } else if (cls[0] == 'C') {
        *r = 1.0; *g = 0.8; *b = 0.2;
    } else {
        *r = 0.2; *g = 0.9; *b = 0.3;
    }
}

/*
 * pic_applet_render_solar - Render the solar weather panel.
 */
double pic_applet_render_solar(cairo_t *cr, double width,
                               time_t now, void *user_data)
{
    pic_solar_data_t *data = (pic_solar_data_t *)user_data;
    double fs = width / 12.0;
    double line_h = fs * 1.4;
    double img_size = width * 0.6;  /* Sun disc takes 60% of panel width */
    double img_y_offset = fs * 1.5; /* Below title */
    double data_y_start = img_y_offset + img_size + fs * 2.5;
    double total_h = data_y_start + line_h * 11;

    (void)now;

    if (!cr) return total_h;

    /* Title */
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0, fs);
    cairo_show_text(cr, "Solar Weather");

    /* Separator */
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, 0, fs + 4);
    cairo_line_to(cr, width, fs + 4);
    cairo_stroke(cr);

    /* Sun disc image — cycles through SDO wavelengths every 60s */
    if (data) {
        pthread_mutex_lock(&data->mutex);
        if (data->sun_image_count > 0) {
            int idx = ((int)(now / 60)) % data->sun_image_count;
            cairo_surface_t *img = data->sun_images[idx];

            if (img) {
                int iw = cairo_image_surface_get_width(img);
                int ih = cairo_image_surface_get_height(img);
                double scale = img_size / (double)iw;
                double ix = (width - img_size) / 2.0;

                cairo_save(cr);
                cairo_translate(cr, ix, img_y_offset);
                cairo_scale(cr, scale, scale * ((double)ih / iw));
                cairo_set_source_surface(cr, img, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);

                /* Wavelength label below the image */
                if (data->sun_labels[idx]) {
                    cairo_text_extents_t ext;
                    double ly = img_y_offset + img_size + fs * 0.3;

                    cairo_set_font_size(cr, fs * 0.65);
                    cairo_text_extents(cr, data->sun_labels[idx], &ext);
                    cairo_set_source_rgba(cr, 0.6, 0.6, 0.7, 0.7);
                    cairo_move_to(cr, (width - ext.width) / 2.0, ly);
                    cairo_show_text(cr, data->sun_labels[idx]);
                }
            }
        }
        pthread_mutex_unlock(&data->mutex);
    }

    /* Data rows */
    if (data) {
        int sfi, wind;
        double kp, bz;
        char xray[8];
        time_t updated;

        /* Read under lock */
        pthread_mutex_lock(&data->mutex);
        sfi = data->sfi;
        kp = data->kp;
        wind = data->solar_wind_speed;
        bz = data->bz;
        strncpy(xray, data->xray_class, sizeof(xray) - 1);
        xray[sizeof(xray) - 1] = '\0';
        updated = data->last_updated;
        pthread_mutex_unlock(&data->mutex);

        cairo_select_font_face(cr, "monospace",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs * 1.0);

        if (sfi > 0) {
            /*
             * Solar indices — two-column layout.
             * Labels left-aligned, values right-aligned at a fixed
             * column position for clean alignment with monospace font.
             */
            /* Helper: draw a label + right-aligned value row */
            #define SOLAR_ROW(row, label_str, val_str, _cr, _r, _g, _b) \
            { \
                double _y = data_y_start + line_h * (row); \
                cairo_set_source_rgba((_cr), (_r), (_g), (_b), 0.85); \
                cairo_move_to((_cr), 0, _y); \
                cairo_show_text((_cr), (label_str)); \
                { \
                    cairo_text_extents_t _ext; \
                    cairo_text_extents((_cr), (val_str), &_ext); \
                    cairo_move_to((_cr), width - _ext.x_advance - 2, _y); \
                    cairo_show_text((_cr), (val_str)); \
                } \
            }

            /* Row 1: SFI */
            {
                double r, g, b;
                char val[16];
                colour_sfi(sfi, &r, &g, &b);
                snprintf(val, sizeof(val), "%d", sfi);
                SOLAR_ROW(0, "SFI", val, cr, r, g, b);
            }

            /* Row 2: Kp */
            {
                double r, g, b;
                char val[16];
                colour_kp(kp, &r, &g, &b);
                snprintf(val, sizeof(val), "%.1f", kp);
                SOLAR_ROW(1, "Kp", val, cr, r, g, b);
            }

            /* Row 3: Solar wind */
            {
                double r, g, b;
                char val[16];
                colour_wind(wind, &r, &g, &b);
                snprintf(val, sizeof(val), "%d km/s", wind);
                SOLAR_ROW(2, "Wind", val, cr, r, g, b);
            }

            /* Row 4: Bz */
            {
                double r, g, b;
                char val[16];
                colour_bz(bz, &r, &g, &b);
                snprintf(val, sizeof(val), "%+.1f nT", bz);
                SOLAR_ROW(3, "Bz", val, cr, r, g, b);
            }

            /* Row 5: X-ray */
            {
                double r, g, b;
                char val[16];
                if (xray[0]) {
                    colour_xray(xray, &r, &g, &b);
                    snprintf(val, sizeof(val), "%s", xray);
                } else {
                    r = 0.5; g = 0.5; b = 0.5;
                    snprintf(val, sizeof(val), "--");
                }
                SOLAR_ROW(4, "X-ray", val, cr, r, g, b);
            }

            #undef SOLAR_ROW

            /* Band condition predictions based on indices.
             *
             * SFI drives which bands ionise:
             *   >150 = 10m excellent, >120 = 15m good, >90 = 20m good
             * Kp degrades propagation:
             *   >5 = severe, >3 = moderate disturbance
             * Bz southward (<-5 nT) triggers storms.
             * M/X flares cause HF blackouts on sunlit side.
             */
            {
                double sep_y = data_y_start + line_h * 5.2;
                double band_y = sep_y + line_h * 0.6;
                int flare_blackout;

                /* Separator */
                cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
                cairo_set_line_width(cr, 0.5);
                cairo_move_to(cr, 0, sep_y);
                cairo_line_to(cr, width, sep_y);
                cairo_stroke(cr);

                /* Section title */
                cairo_select_font_face(cr, "sans-serif",
                                       CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, fs * 0.85);
                cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.8);
                cairo_move_to(cr, 0, band_y);
                cairo_show_text(cr, "Band Conditions");

                /* Check for active flare blackout */
                flare_blackout = (xray[0] == 'M' || xray[0] == 'X');

                cairo_select_font_face(cr, "monospace",
                                       CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, fs * 0.9);

                /* Band condition rows — label left, condition right */
                #define BAND_ROW(row_n, band_str, cond_str, _cr, _r, _g, _b) \
                { \
                    double _y = band_y + line_h * (row_n); \
                    cairo_text_extents_t _ext; \
                    cairo_set_source_rgba((_cr), (_r), (_g), (_b), 0.85); \
                    cairo_move_to((_cr), 0, _y); \
                    cairo_show_text((_cr), (band_str)); \
                    cairo_text_extents((_cr), (cond_str), &_ext); \
                    cairo_move_to((_cr), width - _ext.x_advance - 2, _y); \
                    cairo_show_text((_cr), (cond_str)); \
                }

                /* Low bands (160-40m) */
                {
                    double r, g, b;
                    const char *cond;
                    if (flare_blackout) {
                        cond = "Blackout"; r = 0.9; g = 0.1; b = 0.1;
                    } else if (kp > 5) {
                        cond = "Poor"; r = 0.9; g = 0.3; b = 0.2;
                    } else if (kp > 3) {
                        cond = "Fair"; r = 1.0; g = 0.8; b = 0.2;
                    } else {
                        cond = "Good"; r = 0.2; g = 0.9; b = 0.3;
                    }
                    BAND_ROW(1, "160-40m", cond, cr, r, g, b);
                }

                /* Mid bands (20-17m) */
                {
                    double r, g, b;
                    const char *cond;
                    if (flare_blackout) {
                        cond = "Blackout"; r = 0.9; g = 0.1; b = 0.1;
                    } else if (sfi >= 100 && kp <= 3) {
                        cond = "Good"; r = 0.2; g = 0.9; b = 0.3;
                    } else if (sfi >= 85 && kp <= 5) {
                        cond = "Fair"; r = 1.0; g = 0.8; b = 0.2;
                    } else {
                        cond = "Poor"; r = 0.9; g = 0.3; b = 0.2;
                    }
                    BAND_ROW(2, "20-17m", cond, cr, r, g, b);
                }

                /* High bands (15-10m) */
                {
                    double r, g, b;
                    const char *cond;
                    if (flare_blackout) {
                        cond = "Blackout"; r = 0.9; g = 0.1; b = 0.1;
                    } else if (sfi >= 140 && kp <= 3) {
                        cond = "Good"; r = 0.2; g = 0.9; b = 0.3;
                    } else if (sfi >= 110 && kp <= 4) {
                        cond = "Fair"; r = 1.0; g = 0.8; b = 0.2;
                    } else if (sfi >= 90) {
                        cond = "Poor"; r = 0.9; g = 0.3; b = 0.2;
                    } else {
                        cond = "Closed"; r = 0.5; g = 0.5; b = 0.5;
                    }
                    BAND_ROW(3, "15-10m", cond, cr, r, g, b);
                }

                /* VHF (6m) */
                {
                    double r, g, b;
                    const char *cond;
                    if (kp >= 5 && bz < -5) {
                        cond = "Aurora!"; r = 0.5; g = 0.2; b = 1.0;
                    } else if (sfi >= 170) {
                        cond = "Good"; r = 0.2; g = 0.9; b = 0.3;
                    } else if (sfi >= 140) {
                        cond = "Fair"; r = 1.0; g = 0.8; b = 0.2;
                    } else {
                        cond = "Closed"; r = 0.5; g = 0.5; b = 0.5;
                    }
                    BAND_ROW(4, "6m", cond, cr, r, g, b);
                }

                #undef BAND_ROW
            }

            /* Age note */
            {
                char note[32];
                int age = (int)(now - updated);
                double y = data_y_start + line_h * 10.5;

                if (age < 60) {
                    snprintf(note, sizeof(note), "Updated %ds ago", age);
                } else {
                    snprintf(note, sizeof(note), "Updated %dm ago", age / 60);
                }

                cairo_set_font_size(cr, fs * 0.7);
                cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.6);
                cairo_move_to(cr, 0, y);
                cairo_show_text(cr, note);
            }
        } else {
            /* No data yet */
            double y = data_y_start;
            cairo_set_source_rgba(cr, 0.5, 0.5, 0.6, 0.6);
            cairo_move_to(cr, 0, y);
            cairo_show_text(cr, "Fetching...");
        }
    }

    return total_h;
}
