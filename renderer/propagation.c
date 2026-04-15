/*
 * propagation.c - Shared HF propagation prediction math
 *
 * Single implementation used by both the propagation applet and the
 * propagation heat map layer. Based on MINIMUF 3.5 (McNamara 1982)
 * and ITU-R P.533-14 simplified absorption model.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "propagation.h"
#include <math.h>

/* Effective virtual height for M factor calculation (km).
 * Higher than true hmF2 (~300 km) to account for Earth curvature
 * and the parabolic electron density profile. Gives M(3000) ≈ 3.2,
 * consistent with CCIR ionospheric tables. */
#define HMF2_EFF 490.0

double pic_foF2_estimate(double lat_r, double cos_chi, double ssn)
{
    double lat_abs = fabs(lat_r);

    /* Night floor: latitude-dependent, weakly SSN-dependent.
     * Mid-latitude night foF2 ≈ 2.5-3.0 MHz at low SSN. */
    double night_f0 = 1.8 + 1.0 * cos(lat_abs);
    double night_f1 = 0.012 * ssn * (1.0 + 0.3 * cos(lat_abs));

    /* Daytime maximum: strongly SSN and latitude dependent.
     * Equatorial anomaly peak near +/-15 degrees latitude. */
    double day_lat = 1.0 + 0.3 * cos(2.0 * lat_abs - 0.26);
    double day_f0  = 4.5 * day_lat;
    double day_f1  = 0.035 * ssn * day_lat;

    double foF2_night = night_f0 + night_f1;
    double foF2_day   = day_f0 + day_f1;

    /* Interpolate day/night using cos(chi)^0.6 — empirical exponent
     * from Bradley & Dudeney (1973), used in ITU-R P.533-14. */
    double solar_term = pow(cos_chi, 0.6);
    return foF2_night + (foF2_day - foF2_night) * solar_term;
}

double pic_muf_m_factor(double hop_km)
{
    double half_D = hop_km / 2.0;
    return sqrt(1.0 + (half_D / HMF2_EFF) * (half_D / HMF2_EFF));
}

double pic_calc_luf(double cos_chi, double ssn, int hops, double M)
{
    double threshold = 34.0;  /* dB — 100W dipole with real-world losses */
    double fH = 1.0;          /* Electron gyrofrequency, MHz */
    double K  = 677.0;        /* ITU-R P.533 absorption constant */

    if (cos_chi <= 0.0) return 1.8;  /* No D layer at night */

    /* D-layer absorption ∝ cos(chi)^1.3 * M (ITU-R P.533-14).
     * The M factor accounts for the longer ray path through the
     * D-layer at oblique incidence angles. */
    double luf = sqrt(K * (1.0 + 0.0037 * ssn) *
                      pow(cos_chi, 1.3) * M * hops / threshold) - fH;

    if (luf < 1.8) luf = 1.8;
    if (luf > 30.0) luf = 30.0;
    return luf;
}

double pic_band_reliability(double f, double muf, double luf)
{
    double rel;

    if (muf <= luf || f <= 0.0) return 0.0;

    /* Peak reliability 0.85 — even ideal conditions have day-to-day
     * variability that the median model ignores. */
    rel = 0.85;

    /* Above-MUF roll-off: sharp (layer is penetrated) */
    if (f > muf) {
        double x = (f - muf) / (0.08 * muf);
        rel = 0.85 * exp(-x * x);
    }

    /* Below-LUF roll-off: absorption climbs steeply as 1/f^2 */
    if (f < luf) {
        double x = (luf - f) / (0.15 * luf);
        rel *= exp(-x * x);
    }

    /* Reduce reliability near MUF (within 20%): volatile */
    if (f > 0.80 * muf && f <= muf) {
        double x = (f - 0.80 * muf) / (0.20 * muf);
        rel *= (1.0 - 0.55 * x);
    }

    if (rel < 0.0) rel = 0.0;
    if (rel > 1.0) rel = 1.0;
    return rel;
}

double pic_kp_degrade(double muf, double lat_r, double kp)
{
    if (kp < 4.0) return muf;

    double lat_abs = fabs(lat_r);
    double factor = 1.0 - (kp - 3.0) * 0.08 * (0.3 + 0.7 * sin(lat_abs));
    if (factor < 0.3) factor = 0.3;
    return muf * factor;
}

void pic_gc_midpoint(double lat1, double lon1, double lat2, double lon2,
                     double *mid_lat, double *mid_lon)
{
    double Bx = cos(lat2) * cos(lon2 - lon1);
    double By = cos(lat2) * sin(lon2 - lon1);
    *mid_lat = atan2(sin(lat1) + sin(lat2),
                     sqrt((cos(lat1) + Bx) * (cos(lat1) + Bx) + By * By));
    *mid_lon = lon1 + atan2(By, cos(lat1) + Bx);
}
