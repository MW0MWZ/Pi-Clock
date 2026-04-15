/*
 * propagation.h - Shared HF propagation prediction math
 *
 * Used by both the propagation applet (band-by-hour chart) and the
 * propagation heat map layer. Single source of truth for the
 * ionospheric model — foF2, MUF, LUF, reliability, Kp degradation.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_PROPAGATION_H
#define PIC_PROPAGATION_H

/* Shared propagation geometry constants */
#define PIC_EARTH_RADIUS_KM  6371.0
#define PIC_MAX_HOP_KM       4000.0

/*
 * foF2_estimate - F2 layer critical frequency (MHz).
 *
 *   lat_r   - Geographic latitude of reflection point (radians)
 *   cos_chi - Cosine of solar zenith angle (clamped >= 0)
 *   ssn     - Smoothed sunspot number R12
 */
double pic_foF2_estimate(double lat_r, double cos_chi, double ssn);

/*
 * muf_m_factor - Obliquity factor for F2 reflection.
 *
 *   hop_km - Ground distance of a single hop (km)
 *
 * Returns the M factor (MUF = foF2 * M).
 */
double pic_muf_m_factor(double hop_km);

/*
 * calc_luf - Lowest Usable Frequency for a path.
 *
 *   cos_chi - Cosine of solar zenith angle (>= 0)
 *   ssn     - Smoothed sunspot number
 *   hops    - Number of ionospheric hops
 *   M       - Obliquity factor
 */
double pic_calc_luf(double cos_chi, double ssn, int hops, double M);

/*
 * band_reliability - Predicted reliability for a frequency.
 *
 *   f   - Band centre frequency (MHz)
 *   muf - Path MUF (MHz)
 *   luf - Path LUF (MHz)
 *
 * Returns 0.0 (closed) to 0.85 (best case).
 */
double pic_band_reliability(double f, double muf, double luf);

/*
 * kp_degrade - Degrade MUF based on geomagnetic activity.
 *
 *   muf   - Undegraded MUF (MHz)
 *   lat_r - Latitude of reflection point (radians)
 *   kp    - Current Kp index
 */
double pic_kp_degrade(double muf, double lat_r, double kp);

/*
 * gc_midpoint - Great-circle midpoint (proper spherical geometry).
 *
 * All parameters in radians.
 */
void pic_gc_midpoint(double lat1, double lon1, double lat2, double lon2,
                     double *mid_lat, double *mid_lon);

#endif /* PIC_PROPAGATION_H */
