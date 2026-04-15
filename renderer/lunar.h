/*
 * lunar.h - Lunar position calculations
 *
 * Simplified lunar position algorithm for computing the sub-lunar
 * point and moon phase. Accurate enough for map display purposes.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_LUNAR_H
#define PIC_LUNAR_H

#include <time.h>

/*
 * pic_lunar_position_t - Moon's position relative to Earth.
 *
 *   declination - Lunar declination in radians (north positive)
 *   gha         - Greenwich Hour Angle of the moon in radians
 *   phase       - Moon phase as a fraction (0.0 = new, 0.5 = full)
 */
typedef struct {
    double declination;
    double gha;
    double phase;
} pic_lunar_position_t;

/*
 * pic_lunar_position - Calculate the moon's position for a given time.
 */
pic_lunar_position_t pic_lunar_position(time_t utc);

#endif /* PIC_LUNAR_H */
