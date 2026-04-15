/*
 * display.h - SDL2 display output for Pi-Clock
 *
 * Manages the full-screen SDL2 window and runs the main render
 * loop. On the Raspberry Pi, SDL2 uses the KMSDRM backend to
 * drive the display directly without X11 or Wayland.
 *
 * The render loop:
 *   1. Checks which layers need updating (based on their intervals)
 *   2. Re-renders any stale layers via Cairo
 *   3. Composites all layers onto the SDL2 surface
 *   4. Presents the frame to the display
 *   5. Sleeps until the next update is due
 *
 * The loop runs at a low frame rate (1 FPS by default) since most
 * layers update infrequently. The HUD clock updates every second,
 * which drives the minimum refresh rate. This keeps CPU usage
 * minimal on the Pi Zero W's single-core ARM11.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_DISPLAY_H
#define PIC_DISPLAY_H

#include "layer.h"
#include "borders.h"
#include <cairo/cairo.h>

/*
 * pic_display_run - Start the display and run the render loop.
 *
 * This function initialises SDL2, creates a full-screen window,
 * loads map images, sets up the layer stack, and enters the main
 * render loop. It does not return until the user exits (Ctrl+C,
 * ESC key, or the process receives SIGTERM).
 *
 *   maps_dir - Path to the directory containing map images and
 *              border/timezone data files.
 *   width    - Desired display width in pixels (0 = auto-detect).
 *   height   - Desired display height in pixels (0 = auto-detect).
 *
 * Returns 0 on clean exit, non-zero on error.
 */
int pic_display_run(const char *maps_dir, int width, int height);

/* Shared ticker bar position for legend stacking.
 * Written by the main render loop, read by legend-drawing layers. */
extern double pic_ticker_bar_top;

#endif /* PIC_DISPLAY_H */
