/*
 * math_utils.h - Shared mathematical constants
 *
 * Single source of truth for PI, DEG_TO_RAD, RAD_TO_DEG.
 * Include this instead of defining PI in each translation unit.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_MATH_UTILS_H
#define PIC_MATH_UTILS_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Short alias — many rendering files use bare PI for readability */
#ifndef PI
#define PI M_PI
#endif

#define DEG_TO_RAD  (M_PI / 180.0)
#define RAD_TO_DEG  (180.0 / M_PI)

/* Namespaced aliases for use in headers and public interfaces where
 * short names like DEG_TO_RAD risk collisions with other libraries. */
#define PIC_DEG_TO_RAD DEG_TO_RAD
#define PIC_RAD_TO_DEG RAD_TO_DEG

#endif /* PIC_MATH_UTILS_H */
