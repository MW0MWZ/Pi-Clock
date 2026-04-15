/*
 * lunar.c - Simplified lunar position calculations
 *
 * Computes the moon's position using a low-accuracy algorithm
 * based on Meeus' "Astronomical Algorithms". Accurate to within
 * a few degrees — sufficient for plotting on a world map.
 *
 * The moon's orbit is complex (inclined, elliptical, perturbed
 * by the sun), so this uses the main periodic terms only.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "lunar.h"
#include <math.h>

#include "math_utils.h"
#define TWO_PI (2.0 * M_PI)

static double normalize_angle(double rad)
{
    rad = fmod(rad, TWO_PI);
    if (rad < 0.0) rad += TWO_PI;
    return rad;
}

static double unix_to_julian_date(time_t utc)
{
    return ((double)utc / 86400.0) + 2440587.5;
}

/*
 * pic_lunar_position - Calculate the moon's position.
 *
 * Uses the main terms from Meeus chapter 47 (low accuracy):
 *   - Mean longitude (L')
 *   - Mean anomaly (M')
 *   - Mean elongation (D)
 *   - Argument of latitude (F)
 *   - Plus the largest periodic corrections
 */
pic_lunar_position_t pic_lunar_position(time_t utc)
{
    pic_lunar_position_t pos;
    double jd, t;
    double lp, mp, d, f;
    double moon_lon, moon_lat, ra, dec;
    double obliquity, gmst;
    double sun_m, sun_lon;

    jd = unix_to_julian_date(utc);
    t = (jd - 2451545.0) / 36525.0;

    /* Mean elements of the moon's orbit */
    lp = normalize_angle((218.3165 + 481267.8813 * t) * DEG_TO_RAD);
    mp = normalize_angle((134.9634 + 477198.8676 * t) * DEG_TO_RAD);
    d  = normalize_angle((297.8502 + 445267.1115 * t) * DEG_TO_RAD);
    f  = normalize_angle((93.2720 + 483202.0175 * t) * DEG_TO_RAD);
    /* Sun's mean anomaly (needed for some corrections) */
    sun_m = normalize_angle((357.5291 + 35999.0503 * t) * DEG_TO_RAD);

    /*
     * Ecliptic longitude corrections (main terms).
     * These are the largest periodic perturbations to the
     * moon's mean longitude.
     */
    moon_lon = lp
        + 6.289 * DEG_TO_RAD * sin(mp)           /* equation of centre */
        + 1.274 * DEG_TO_RAD * sin(2.0 * d - mp) /* evection */
        + 0.658 * DEG_TO_RAD * sin(2.0 * d)      /* variation */
        - 0.186 * DEG_TO_RAD * sin(sun_m)         /* annual equation */
        - 0.114 * DEG_TO_RAD * sin(2.0 * f);      /* reduction to ecliptic */

    moon_lon = normalize_angle(moon_lon);

    /* Ecliptic latitude (main term) */
    moon_lat = 5.128 * DEG_TO_RAD * sin(f);

    /* Obliquity of the ecliptic */
    obliquity = (23.439291 - 0.0130042 * t) * DEG_TO_RAD;

    /* Convert ecliptic to equatorial (RA, Dec) */
    ra = atan2(cos(obliquity) * sin(moon_lon) - tan(moon_lat) * sin(obliquity),
               cos(moon_lon));
    dec = asin(sin(obliquity) * sin(moon_lon) * cos(moon_lat)
              + cos(obliquity) * sin(moon_lat));

    /* Greenwich Mean Sidereal Time */
    gmst = normalize_angle((280.46061837
                            + 360.98564736629 * (jd - 2451545.0))
                           * DEG_TO_RAD);

    /* Greenwich Hour Angle */
    pos.declination = dec;
    pos.gha = normalize_angle(gmst - ra);

    /*
     * Moon phase calculation.
     * The phase angle is approximately the elongation (D) of the
     * moon from the sun. Phase fraction:
     *   0.0 = new moon (sun and moon aligned)
     *   0.5 = full moon (180 degrees apart)
     *
     * We also need the sun's ecliptic longitude for accurate phase.
     */
    sun_lon = normalize_angle((280.46646 + 36000.76983 * t) * DEG_TO_RAD
              + (1.9146 * sin(sun_m)) * DEG_TO_RAD);

    {
        double elongation = moon_lon - sun_lon;
        /* Phase = (1 - cos(elongation)) / 2
         * This gives 0 at new moon, 1 at full moon */
        pos.phase = (1.0 - cos(elongation)) / 2.0;
    }

    return pos;
}
