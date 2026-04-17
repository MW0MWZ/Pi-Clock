/*
 * image.h - Image loading utilities for Pi-Clock
 *
 * Provides functions to load JPEG and PNG images from disk and
 * convert them into Cairo image surfaces ready for compositing.
 *
 * Uses stb_image.h (public domain, by Sean Barrett) for decoding.
 * This avoids adding libjpeg/libpng as build dependencies — the
 * single header file handles everything.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_IMAGE_H
#define PIC_IMAGE_H

#include <cairo/cairo.h>

/*
 * pic_image_load - Load an image file into a Cairo surface.
 *
 * Reads a JPEG or PNG file from disk, decodes it, and creates a
 * Cairo ARGB32 image surface with the pixel data. The surface
 * dimensions match the source image.
 *
 * The caller is responsible for destroying the returned surface
 * with cairo_surface_destroy() when it's no longer needed.
 *
 *   path - File path to the image (JPEG or PNG).
 *
 * Returns a Cairo image surface on success, or NULL on failure
 * (file not found, decode error, or memory allocation failure).
 */
cairo_surface_t *pic_image_load(const char *path);

/*
 * pic_image_load_scaled - Load an image and scale it to target dimensions.
 *
 * Same as pic_image_load(), but after loading the source image, it
 * creates a second surface at the requested dimensions and paints
 * the source onto it with bilinear filtering. The source surface
 * is freed internally.
 *
 * This is used to match map images to the display resolution. For
 * example, loading a 21600x10800 Blue Marble JPEG and scaling it
 * to 1920x1080 for a 1080p display.
 *
 *   path   - File path to the image (JPEG or PNG).
 *   width  - Target width in pixels.
 *   height - Target height in pixels.
 *
 * Returns a Cairo image surface at the requested dimensions on
 * success, or NULL on failure.
 */
cairo_surface_t *pic_image_load_scaled(const char *path,
                                       int width, int height);

/*
 * pic_map_load - Load a world map image with optional hi-res upgrade.
 *
 * Builds the path <maps_dir>/<folder>/<basename>_<suffix>.jpg where
 * suffix is chosen from the display resolution (720p/1080p/1440p/4k).
 *
 * When allow_hires is non-zero and the display is below 4K, first
 * try to load the _4k variant at its native resolution (via
 * pic_image_load, no downscaling). This keeps the extra source
 * detail available for zoom painting — a 1080p display with a 4k
 * Blue Marble source samples ~2x more source pixels per screen
 * pixel than loading the matched-res 1080p file.
 *
 * If the 4k upgrade fails or is disabled, fall back to the matched
 * resolution file via pic_image_load_scaled(display_w, display_h),
 * which is the pre-zoom behaviour.
 *
 * allow_hires is the caller's RAM gate: the 4k surface is ~33 MB
 * vs. ~8 MB at 1080p, so pass 0 on Pi Zero/Pi 1 class devices
 * (< ~768 MB RAM). On desktop dev hosts always pass 1.
 *
 *   maps_dir    - Root of the maps directory.
 *   folder      - Subdirectory (e.g. "blue_marble" or "black_marble").
 *   basename    - Filename prefix (e.g. "blue_marble").
 *   display_w,
 *   display_h   - Target display dimensions in pixels.
 *   allow_hires - Non-zero to prefer the 4k source when available.
 *
 * Returns a Cairo surface (at either native 4k or display size), or
 * NULL if no file could be loaded.
 */
cairo_surface_t *pic_map_load(const char *maps_dir,
                              const char *folder,
                              const char *basename,
                              int display_w, int display_h,
                              int allow_hires);

#endif /* PIC_IMAGE_H */
