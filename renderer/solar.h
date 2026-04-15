/*
 * solar.h - Solar position and day/night terminator calculations
 *
 * This module computes the sun's position in the sky and the boundary
 * between day and night (the "terminator" or "greyline") using standard
 * astronomical formulas. All calculations are done with basic trig —
 * no external libraries or network lookups required.
 *
 * The key concept is the "sub-solar point": the spot on Earth's surface
 * where the sun is directly overhead. This point moves westward as Earth
 * rotates (giving us day/night) and north/south with the seasons (giving
 * us longer/shorter days). The terminator is the circle on Earth's surface
 * that is exactly 90 degrees away from the sub-solar point.
 *
 * Reference: Jean Meeus, "Astronomical Algorithms" (2nd ed., 1998)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_SOLAR_H
#define PIC_SOLAR_H

#include <time.h>

/*
 * pic_solar_position_t - The sun's position relative to Earth.
 *
 * These two values together define the sub-solar point, which is all
 * we need to compute the day/night boundary anywhere on the globe.
 *
 *   declination - The sun's latitude: how far north (+) or south (-)
 *                 of the equator the sun appears. Ranges from about
 *                 -23.44 (winter solstice) to +23.44 (summer solstice).
 *                 Measured in radians.
 *
 *   gha         - Greenwich Hour Angle: the sun's "longitude" measured
 *                 westward from the Prime Meridian. This tells us where
 *                 on Earth the sun is currently overhead, and it advances
 *                 about 15 degrees per hour as Earth rotates.
 *                 Measured in radians (0 to 2*PI).
 */
typedef struct {
    double declination;     /* Solar declination in radians              */
    double gha;             /* Greenwich Hour Angle in radians           */
} pic_solar_position_t;

/*
 * pic_solar_position - Calculate the sun's position for a given time.
 *
 * Uses a simplified solar position algorithm accurate to within about
 * 1 arcminute — more than sufficient for drawing the terminator on a
 * world map where individual pixels span many arcminutes.
 *
 *   utc - Current time as UTC unix timestamp.
 *
 * Returns an pic_solar_position_t with declination and GHA in radians.
 */
pic_solar_position_t pic_solar_position(time_t utc);

/*
 * pic_solar_illumination - Calculate daylight fraction for a map pixel.
 *
 * Given a latitude/longitude and the sun's current position, returns
 * a value indicating how illuminated that point is:
 *
 *   1.0 = full daylight (sun well above horizon)
 *   0.0 = full night (sun well below horizon)
 *   0.0 - 1.0 = twilight zone (smooth gradient at the terminator)
 *
 * The twilight gradient is cosmetic (about 6 degrees wide) to give
 * the terminator a realistic soft edge rather than a hard line.
 *
 *   lat_rad - Latitude in radians (north positive).
 *   lon_rad - Longitude in radians (east positive).
 *   sun     - Current solar position from pic_solar_position().
 */
double pic_solar_illumination(double lat_rad, double lon_rad,
                              const pic_solar_position_t *sun);

#endif /* PIC_SOLAR_H */
