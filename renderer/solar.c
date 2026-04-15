/*
 * solar.c - Solar position and day/night terminator calculations
 *
 * Implements a simplified solar position algorithm based on the
 * equations in Jean Meeus' "Astronomical Algorithms". The approach:
 *
 *   1. Convert the unix timestamp to Julian Date (days since
 *      January 1, 4713 BC in the Julian calendar).
 *   2. From the Julian Date, compute the number of Julian centuries
 *      since J2000.0 (January 1, 2000, 12:00 UTC).
 *   3. Use this to calculate the sun's ecliptic longitude.
 *   4. Convert ecliptic longitude to right ascension and declination.
 *   5. Combine with Earth's rotation to get the Greenwich Hour Angle.
 *
 * The result tells us exactly where on Earth the sun is directly
 * overhead at any given moment.
 *
 * Accuracy: within ~1 arcminute, which is far better than needed
 * for a world map display where pixels are many arcminutes wide.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "solar.h"
#include <math.h>

#include "math_utils.h"
#define TWO_PI (2.0 * M_PI)

/*
 * normalize_angle - Wrap an angle in radians to the range [0, 2*PI).
 *
 * Many astronomical calculations produce angles outside this range
 * (e.g., accumulated rotations). This wraps them back without
 * changing the actual direction they represent.
 */
static double normalize_angle(double rad)
{
    rad = fmod(rad, TWO_PI);
    if (rad < 0.0) {
        rad += TWO_PI;
    }
    return rad;
}

/*
 * unix_to_julian_date - Convert a Unix timestamp to Julian Date.
 *
 * The Julian Date is a continuous count of days since the beginning
 * of the Julian period (January 1, 4713 BC). It's the standard
 * time scale in astronomy because it avoids all the complexity of
 * calendars, leap years, and time zones.
 *
 * The Unix epoch (January 1, 1970 00:00 UTC) corresponds to
 * Julian Date 2440587.5. We add the elapsed days since then.
 */
static double unix_to_julian_date(time_t utc)
{
    return ((double)utc / 86400.0) + 2440587.5;
}

/*
 * pic_solar_position - Calculate the sun's position for a given time.
 *
 * This follows the "low accuracy" solar position algorithm from Meeus,
 * which uses the sun's mean anomaly and a first-order correction
 * (equation of center) to find the ecliptic longitude. The steps:
 *
 *   1. Compute T = Julian centuries since J2000.0
 *   2. Find the sun's mean longitude (L0) and mean anomaly (M)
 *   3. Apply the equation of center to get true longitude
 *   4. Compute the obliquity of the ecliptic (Earth's axial tilt)
 *   5. Convert ecliptic coordinates to equatorial (RA, Dec)
 *   6. Compute the Greenwich Mean Sidereal Time (GMST)
 *   7. GHA = GMST - RA (the angle from Greenwich to the sub-solar point)
 */
pic_solar_position_t pic_solar_position(time_t utc)
{
    pic_solar_position_t pos;
    double jd, t, l0, m, c, sun_lon, obliquity, ra, dec, gmst;

    /* Step 1: Julian centuries since J2000.0 (Jan 1, 2000 12:00 UTC).
     * J2000.0 is Julian Date 2451545.0. Dividing by 36525 converts
     * days to Julian centuries (each exactly 36525 days). */
    jd = unix_to_julian_date(utc);
    t = (jd - 2451545.0) / 36525.0;

    /* Step 2: Mean longitude of the sun (L0) and mean anomaly (M).
     * L0 is where the sun would be if Earth's orbit were circular.
     * M measures how far along its elliptical orbit Earth has travelled.
     * Both advance steadily with time. */
    l0 = normalize_angle((280.46646 + 36000.76983 * t) * DEG_TO_RAD);
    m  = normalize_angle((357.52911 + 35999.05029 * t) * DEG_TO_RAD);

    /* Step 3: Equation of center — correction for orbital eccentricity.
     * Earth's orbit is slightly elliptical, so the sun appears to speed
     * up and slow down. This sine series approximates the difference
     * between mean and true anomaly. */
    c = (1.9146 * sin(m) + 0.02 * sin(2.0 * m)) * DEG_TO_RAD;

    /* True ecliptic longitude: where the sun actually is on the ecliptic */
    sun_lon = normalize_angle(l0 + c);

    /* Step 4: Obliquity of the ecliptic — Earth's axial tilt.
     * Currently about 23.44 degrees, decreasing very slowly (about
     * 0.013 degrees per century due to gravitational interactions
     * with other planets). */
    obliquity = (23.439291 - 0.0130042 * t) * DEG_TO_RAD;

    /* Step 5: Convert ecliptic to equatorial coordinates.
     *
     * The ecliptic is the plane of Earth's orbit. The equatorial
     * system is aligned with Earth's equator. The conversion uses
     * the obliquity as the rotation angle between these two planes.
     *
     * Right Ascension (RA): the celestial equivalent of longitude
     * Declination (Dec): the celestial equivalent of latitude
     */
    ra = atan2(cos(obliquity) * sin(sun_lon), cos(sun_lon));
    dec = asin(sin(obliquity) * sin(sun_lon));

    /* Step 6: Greenwich Mean Sidereal Time (GMST).
     * Sidereal time measures Earth's rotation relative to the stars
     * (not the sun). It tells us which right ascension is currently
     * on the meridian at Greenwich. The formula gives GMST in degrees,
     * which we convert to radians. */
    gmst = normalize_angle((280.46061837
                            + 360.98564736629 * (jd - 2451545.0))
                           * DEG_TO_RAD);

    /* Step 7: Greenwich Hour Angle = GMST - RA.
     * This gives us the angle between Greenwich and the sub-solar
     * point, measured westward. Combined with declination, it
     * pinpoints where on Earth the sun is directly overhead. */
    pos.declination = dec;
    pos.gha = normalize_angle(gmst - ra);

    return pos;
}

/*
 * pic_solar_illumination - Calculate daylight fraction for a point.
 *
 * The core idea: a point on Earth is in daylight when the angle
 * between it and the sub-solar point (measured from Earth's center)
 * is less than 90 degrees.
 *
 * We compute this using the spherical law of cosines:
 *
 *   cos(zenith) = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(ha)
 *
 * Where:
 *   lat = observer's latitude
 *   dec = sun's declination (from pic_solar_position)
 *   ha  = local hour angle (how far east/west the observer is from
 *          the sub-solar meridian)
 *
 * cos(zenith) > 0  means the sun is above the horizon (daytime)
 * cos(zenith) < 0  means the sun is below the horizon (nighttime)
 * cos(zenith) = 0  means the sun is exactly on the horizon
 *
 * We smooth the transition with a gradient across the twilight zone
 * (about 6 degrees either side of the terminator) to give the
 * display a realistic soft edge rather than an abrupt day/night line.
 */
double pic_solar_illumination(double lat_rad, double lon_rad,
                              const pic_solar_position_t *sun)
{
    double ha, cos_zenith, illumination;

    /*
     * Local hour angle: how far the observer is from the sub-solar
     * meridian. We subtract GHA (which measures westward from Greenwich)
     * from the observer's longitude (east positive) to get the angular
     * distance between observer and sun.
     */
    ha = lon_rad + sun->gha;

    /*
     * Spherical law of cosines for the solar zenith angle.
     * This single formula encodes both the seasonal effect (declination)
     * and the time-of-day effect (hour angle).
     */
    cos_zenith = sin(lat_rad) * sin(sun->declination)
               + cos(lat_rad) * cos(sun->declination) * cos(ha);

    /*
     * Convert the hard terminator into a smooth gradient with
     * physically-based width.
     *
     * The width of the twilight zone depends on the angle at which
     * the sun's daily path crosses the horizon. This angle is
     * determined by both the observer's latitude and the sun's
     * declination:
     *
     *   crossing_rate = cos(latitude) * cos(declination)
     *
     * When this product is large (equator at equinox), the sun
     * plunges steeply through the horizon → narrow, sharp terminator.
     *
     * When it's small (high latitude in summer), the sun skims
     * along almost parallel to the horizon → very wide, gradual
     * twilight zone. This is why Arctic summers have hours of
     * twilight while tropical sunsets are over in minutes.
     *
     * We use the inverse of crossing_rate to scale the gradient:
     *
     *   gradient = base_sharpness / crossing_rate
     *
     * Clamped to prevent extreme values at the poles where the
     * product approaches zero (midnight sun / polar night regions).
     *
     * Tuned values:
     *   base = 0.035 → equatorial equinox: ~4° band (crisp)
     *   min crossing = 0.08 → prevents infinite width
     *   max gradient = 0.20 → polar summer: ~23° band (wide)
     */
    {
        double cos_lat = cos(lat_rad);
        double cos_dec = cos(sun->declination);
        double crossing_rate = fabs(cos_lat * cos_dec);
        double gradient;

        /* Clamp to prevent extreme widths at poles during solstice */
        if (crossing_rate < 0.08) {
            crossing_rate = 0.08;
        }

        gradient = 0.035 / crossing_rate;

        /* Cap the maximum gradient width */
        if (gradient > 0.20) {
            gradient = 0.20;
        }

        illumination = (cos_zenith + gradient) / (2.0 * gradient);
    }

    /* Clamp to [0, 1] */
    if (illumination < 0.0) illumination = 0.0;
    if (illumination > 1.0) illumination = 1.0;

    return illumination;
}
